## v1.0.59 -- controller: Field Orders back/close fixed

- Fixed the Field Orders board breaking on a controller when the game switches
  between keyboard and gamepad input (frequent on Steam Deck): B would stop working
  as back/close, navigation got erratic, and only keyboard Esc could close the
  board. B -- and all controller nav -- now runs entirely off the game's own
  controller input stream instead of a separate poll that went deaf on a mode
  flip, so back and close stay reliable through keyboard/gamepad switches, docked
  or handheld. B also responds on the first press every time you reopen the board.

## v1.0.58 -- concentration spells: bounded, and no more freeze

- Fixed a hard freeze (the game hangs, no crash log) that could strike when a
  follower ran a cast gambit for a concentration spell like Flames. The spell
  became a permanent held stream that sprayed a nearby teammate; that friendly
  fire made the two followers fight, and the "followers stop fighting each other"
  safety then deadlocked the game trying to break up the endless re-aggro. The
  whole chain is closed -- the ally-combat quash can no longer lock up, and the
  stream that started it is bounded (below).
- Concentration spells now cast PROPERLY BOUNDED instead of streaming forever:
  hostile streams (Flames, Frostbite, Sparks) hold ~1-4 seconds, healing streams
  hold until the target is topped off, utility holds while it's relevant -- each
  with a clean cut-off. A follower never rakes an ally: the stream won't start,
  and cuts out mid-cast, if a teammate (or you) crosses its line of fire.
- At the "Exact spell" cast-control setting, concentration spells are now held to
  the same rule as every other spell -- no spell class can slip the exact bound.
  Lower cast-control settings leave a follower's own concentration casting alone.

## v1.0.53 -- combat pathing fix: melee/ranged followers stop moving like casters

- Fix: a follower forced to melee or ranged now MOVES like one. MFO's combat-style
  records were copying their close-range positioning (how tightly to circle a foe,
  how readily to fall back) byte-for-byte from a vanilla MAGE style, so a
  sword-and-board or archer follower kited and back-pedalled like a caster even
  though it was picking the right attacks -- which is why battle movement felt
  worse than vanilla. Each style now carries its MATCHING vanilla positioning:
  melee circles in close and holds ground, ranged keeps a bowman's spacing, casters
  are unchanged. ESP-only fix; no settings to touch.

## v1.0.52 -- loose loot: followers actually PICK IT UP now

- Fix: a follower who walks to loose gold/ammo now actually collects it. The
  pickup was dispatched through a Papyrus VM Activate call that silently failed on
  every gold pile (field: the follower reached the gold, logged `ACTIVATE dispatch
  FAILED` nine times, and never grabbed any). MFO now uses the engine's NATIVE
  activate -- the exact thing that happens when YOU press the activate key -- run
  on the main thread so it stays safe off the worker. (v1.0.50 raised the pickup
  reach so he gets close enough; this makes the grab itself work. Tunable
  fLooseAcquireDist still applies.)

## v1.0.51 -- un-equippable-gear eviction now covers ALL slots + weapons (not just heads)

- Fix (#62 follow-up): the "take off gear that can't be equipped" fix now covers
  EVERY worn slot, not just the head. Creature/non-playable ARMOR is deleted and
  creature WEAPONS are swapped out in any slot/hand (both were already the case);
  and a foreign wrong-race piece that renders NOTHING is now handed back to you
  from any VISIBLE slot -- chest, body, hands, feet, shield, head -- so an
  invisible chest piece is corrected the same way an invisible head is. Rings and
  amulets are deliberately left alone (their render test is unreliable on custom
  races and an invisible ring is negligible; a non-playable creature ring is still
  deleted anywhere).

## v1.0.50 -- combat-class override; "when dark" condition; loose-loot pickup fix

- New (#65): a per-follower COMBAT CLASS override on the board -- set a follower to
  Melee / Ranged / Mage, or leave it Auto. Auto is the default and changes nothing,
  so a follower you don't reclass is never touched (custom followers stay safe). An
  override forces the combat stance, superseding whatever the gambits inferred.
- New: a "When dark" gambit condition (dark = interior/dungeon OR night) -- so you
  can author "when dark -> cast Magelight". (The existing "In an interior" and "At
  night" conditions remain; this is just the combined convenience in one rule.)
- Fix: followers no longer get stuck cycling reachable loose gold/ammo without ever
  picking it up. Loose piles are now grabbed from a longer, tunable reach
  (fLooseAcquireDist, default 300) -- the engine's pickup is distance-independent,
  so a follower who walked as close as the navmesh allows now grabs the pile
  instead of stalling just short of it. A pile that's genuinely out of reach or
  won't pick up is abandoned cleanly instead of looping forever.

## v1.0.49 -- followers keep their own gear; cast gambits target smarter

- Fix (#69): MFO no longer gives away a follower's OWN gear. A custom follower's
  signature weapon (Feris's Gauldurbow) or a 1h/bow hybrid's off-hand sidearm was
  being handed to the player whenever it read as "off-role." MFO now snapshots a
  follower's gear the first time it manages them and NEVER sheds or displaces that
  stock gear -- only gear MFO itself looted is ever managed. And the loot and
  hand-back-off-role systems now judge a follower's weapon role from ONE stable
  signal (their gambits + what they actually carry), not whatever weapon is drawn
  that instant -- so they can't disagree and thrash (loot a 1h, then shed it while
  the bow is out). Pure casters are unchanged (still just their one sidearm).
- New (#68): cast gambits target smarter. A cast whose target is "questionable" --
  e.g. "when dark -> cast Magelight" with no foe around -- now falls through to
  YOU (the player) and lights up, instead of failing to cast at all. Cast rows get
  a Target picker: Auto (the foe/ally if any, else you), the player by name,
  Nearest ally, or a specific follower by name. A cast aimed at an obvious target
  that's beyond the spell's range is skipped cleanly so the rule below it still runs.

## v1.0.48 -- LOTD drop-off boxes left alone (towns/inns/museum); SE 1.5.97 cast-control crash gated

- Fix (#66): followers no longer loot from Legacy of the Dragonborn drop-off /
  income / sell / shipment boxes. The reported case -- the drop-off boxes in
  TOWNS and INNS -- is matched by the container base form (they sit in ordinary
  public cells with no owner/keyword, so nothing else caught them) and skipped
  UNCONDITIONALLY; inert if LOTD isn't installed. Separately, containers in the
  player's own space (the museum halls + every vanilla/Hearthfire/mod player
  home) are skipped too, detected by LocTypePlayerHouse + player ownership and
  governed by the existing bLootInPlayerHomes toggle (default OFF).
- Fix (#67): a crash on Skyrim Special Edition 1.5.97 when the cast-control slider
  was in use. The mage cast-control path is AE-developed and faulted on SE 1.5.97
  (crash log pinned it to the follower service tick -> Fire -> CastOn); it's now
  gated to AE only, mirroring the existing VR guards. On SE/VR, mage followers
  cast through their own vanilla AI instead -- no crash, graceful fallback. AE
  (the primary target) is unaffected.

## v1.0.46 -- invisible head: the ROOT CAUSE (creature/draugr armor eviction)

- Fix: a follower's head (or another slot) going invisible. ROOT CAUSE, this time
  verified at the save + plugin-record level: MFO had looted a NON-PLAYABLE
  creature item off a corpse -- e.g. a draugr's helmet -- and equipped it. Those
  items render on no playable race, so they hide the slot but draw nothing (Inigo's
  "Ancient Nord Helmet" was in fact a draugr helmet, count 2, worn). It's the armor
  version of the old creature-WEAPON bug, is NOT beast-specific, and has nothing to
  do with 3D rebuilds -- the whole v1.0.39-45 line of fixes was chasing the wrong
  layer. MFO now, on load and after any equip/trade, takes OFF worn gear that
  renders nothing on the follower's race: creature/draugr junk is DELETED, a
  foreign wrong-race helmet is HANDED BACK to you, and a custom follower's OWN
  intentional invisible gear is left untouched. (Prevention was already in place --
  MFO stopped looting non-playable armor back in v1.0.41 -- so this is the cleanup
  for gear a pre-fix build stuck on, plus trades.) Toggle bBeastHeadFix. Watch
  "[evict]" in MFO.log.

## v1.0.45 -- phantom head-item cleaner (fixes beast-race headless)

- New: MFO now identifies and clears "phantom" head gear -- a worn head-slot item
  that renders nothing on the follower's race and doesn't belong to the follower's
  own mod (e.g. a stuck vanilla helmet on a custom Khajiit). That's the invisible-
  head cause. A custom follower's OWN intentional invisible gear is identified by
  plugin ownership and left completely untouched. Runs on load and after equips,
  beast-race followers, toggle bBeastHeadFix. Watch "[phantom]" in MFO.log.

## v1.0.44 -- beast-race headless: route trade-equips through MFO's safe path

- Fix: beast-race followers (Khajiit/Argonian, e.g. Inigo) going headless when you
  TRADE them gear. MFO's own looting already equips on a safe path that never drops
  the head; the trade path didn't get that treatment. MFO now re-does a traded
  piece's equip through that same safe path. Inventory-hidden native items (a custom
  follower's own invisible gear) are left completely untouched. Toggle bBeastHeadFix.

## v1.0.43 -- beast-race headless: the real fix (light repair, never a forced rebuild)

- Fix: beast-race followers (Khajiit/Argonian, incl. custom ones like Inigo) going
  headless. Root cause: MFO was FORCING a heavy 3D rebuild on equip, which is
  exactly what detaches a beast follower's head (vanilla never does this, which is
  why it was an MFO-only problem). MFO now does the opposite -- it only acts when a
  head is ALREADY missing, and then uses the light reattach the community headless-
  NPC fixer uses (no forced rebuild, no items touched, healthy followers untouched).
  Runs on load and after equips. Toggle bBeastHeadFix.

## v1.0.42 -- followers stop fighting each other; stronger beast-head rebuild

- New: followers no longer fight EACH OTHER. If one follower ever aggros another
  (usually from a stray area-of-effect spell), MFO immediately ends that fight on
  both sides. Toggle bQuashAllyCombat.
- Fix: strengthened the beast-race head rebuild (a v1.0.41 follow-up) -- the reset
  now does a full rebuild, which the lighter one didn't. Plus a diagnostic that
  logs whether a worn piece lacks a mesh for the follower's race, to pin any
  remaining case.

## v1.0.41 -- beast heads fix themselves on load; stop looting creature "skins"

- Fix: a beast-race follower who loads a save with a broken/invisible head now has
  it rebuilt automatically on load -- no need to trade or loot to trigger the fix.
  So an existing save with a headless follower comes up correct.
- Fix: MFO no longer loots non-playable creature "skin" armor (e.g. More Nasty
  Critters' "BearBrownSoft") -- these carry an armor rating and a body slot but are
  a creature's invisible body, not gear a follower can wear. Same non-playable
  check the mod already uses for creature weapons.

## v1.0.40 -- beast-race head fix now covers TRADING and AI re-equips, not just looting

- Fix: a beast-race follower (Khajiit/Argonian, e.g. Inigo) still lost their head
  when you TRADED them a weapon/armor, even though MFO's own looting no longer
  broke it (v1.0.39). The head rebuild now fires on the equip EVENT itself, so it
  covers every way gear gets equipped -- MFO looting, you handing/trading gear, or
  the follower's own AI re-dressing. Runs out of combat only (no in-combat
  flicker), just for beast-race followers. Still toggleable with bBeastHeadFix.

## v1.0.39 -- fix: beast-race followers (Khajiit/Argonian) no longer go headless

- Fix: Khajiit/Argonian followers (including custom ones like Inigo) losing their
  HEAD when MFO equipped looted gear on them. Beast-race heads detach when
  equipment is changed by a script and have to be explicitly rebuilt; MFO now
  forces that head rebuild right after equipping on a beast-race follower. Only
  beast races are touched (human followers are unaffected), and it can be turned
  off with bBeastHeadFix = 0 in the INI. Builds on the v1.0.38 main-thread equip
  fix below.

## v1.0.38 -- fix: followers no longer lose their head when they equip armor

- Fix: a follower's HEAD (or body) disappearing after MFO put a piece of armor on
  them. MFO was equipping looted gear from a background thread, and the engine's
  head/body 3D rebuild has to happen on the main thread -- off it, the head node
  was torn down and never rebuilt. It happened with perfectly good armor (even
  plain chainmail), because the bug was in HOW the armor was equipped, not the
  armor itself. MFO now equips looted armor and weapons on the main thread.
  (This also removes one likely source of graphics/memory crashes.)

## v1.0.35 -- casters obey "exact", and stop firing into your allies' backs

- Fix: "Cast control: Exact spell" (and the other levels) now actually STOPS a
  follower's own spells. The old hook only ADVISED the AI not to cast -- it still
  slipped spells through (a latched mage cast Icy Shard / Mage Armor in "exact").
  MFO now hard-aborts a denied cast at the moment it fires, via the engine's own
  interrupt (magicka refunded, no freeze). Watch "[consent] ... HARD-ABORTED".
- Fix: Absorb Health (and other drain/absorb spells) count as OFFENSE, not heals,
  so they obey "ignore buffs & heals". Spell category is now read from the spell's
  own effects -- anything that harms a target is offense -- instead of guessing
  from which caster the engine happened to use.
- New: friendly-fire hold (default ON, INI bFriendlyFireHold). Followers no longer
  fire offensive spells into a teammate standing in the line or blast -- the mage
  holds the cast instead (the game's line-of-sight check ignores allies, so this
  was firing Fireballs into a follower's back in a corridor). A decay valve lets a
  shot through after several holds so a mage in a tight formation can't be muted.
  The gambit's own chosen spell is never held. Watch "... (friendly fire)".

## v1.0.37 -- mage follow-up: cast control that sticks, out-of-combat casting that works

- Fix: "Cast control" now actually STOPS a follower's own spells at the level you
  set. The wrong spell is denied BEFORE it charges (no wrong-spell cast animation),
  and MFO keeps the gambit spell in their hand so they cast what you told them --
  no more standing there with the wrong spell in hand doing nothing.
- Fix: Absorb Health (and other drain/absorb spells) count as OFFENSE, not heals,
  so they obey "ignore buffs & heals". Spell category is read from the spell's own
  effects now, not guessed from which caster the engine happened to use.
- Fix: cast-in-logistics (out-of-combat cast gambits) now fires. Self casts apply
  the effect directly (the forced-package route can't deliver self-only spells);
  a foe target uses the animated package. Candlelight/Magelight at night and
  out-of-combat self-buffs/heals all work now, paced by the spell's own duration.
- New: "Cast on player" logistics action -- the follower applies a spell's effect
  to YOU (e.g. Candlelight, so the light follows the player).

## v1.0.34 -- the mage update: full cast control, casting out of combat, spell teaching

- New: CAST CONTROL SLIDER (MCM, "Cast control", default "Ignore heals"). Graduates
  how tightly a follower's own spellcasting is overridden toward the gambit's spell:
  Off (no control) -> Ignore buffs & heals (force the gambit for offense, let them
  buff/heal) -> Ignore heals -> Ignore self-heals -> Exact spell (only the gambit
  spell is ever cast). The consent hook classifies each of the AI's own spells by
  the engine's own caster category and denies just the ones your slider level says.
- New: MFO_CastStyle -- while a cast gambit owns a follower, MFO swaps their live
  combat style to a pure-mage one so their AI actually casts (and stays mobile),
  and swaps them to pure MELEE the moment magicka runs dry, back to caster when it
  regenerates. Cast till dry, then steel. Rides the same proven combat-style rails
  as v1.0.33's weapon stances.
- New: CAST IN LOGISTICS -- cast gambits (cast on self / at foe-ally) now run in the
  logistics table too, so a follower can self-buff, light a candle, or heal out of
  combat as upkeep. A still-active self-buff is not re-cast.
- New: TEACH SPELLS FROM SPELLBOOKS -- the gambit spell picker now also lists spells
  you carry a spellbook for that the follower does not know, as "Name (spellbook)".
  A second click (warned: it DESTROYS the book) teaches the spell and sets it as the
  gambit's spell in one step.
- New: MCM "Fashionrim" toggle (default off) -- stop MFO acquiring and fitting ARMOR
  (armor looting, mage robe loadout, auto-equip, gem transfer, armor buying) so you
  dress followers by hand. Selling still works (worn/socketed gear is never sold);
  weapons are entirely unaffected.
- New: MCM combat-HUD position sliders -- nudge the combat overlay clear of another
  mod's HUD (right/top margin in pixels), clamped on-screen.
- Fix (#55): MCM toggles added in an update now REGISTER on existing saves. MFO now
  ships the MCM Helper defaults file (MCM/Config/MFO/settings.ini) it always should
  have -- the mutable user store MO2 was shadowing with a stale copy was never the
  registration source. A new release gate (audit_mcm.py) blocks shipping any MCM
  control that isn't wired in all five places.

## v1.0.33 -- weapon-stance ownership: followers hold the stance their gambit picked

- Fix: an archer told to switch to melee at close range (Auri) would flicker
  bow<->mace and never attack -- "switch back and forth without doing anything."
  Root cause was a tug-of-war: MFO's act.equip_melee gambit forced the mace into
  her hands, but her OWN combat AI -- weighted ranged -- re-drew the bow the
  instant the equip's suppression window lapsed, so every action window was
  eaten by a re-equip and she never reached the attack rule. A one-shot
  EquipObject cannot win against a ranged-forward combat style.
- New: WEAPON-STANCE OWNERSHIP (bWeaponStyleControl, default ON). When an equip
  gambit wins a follower's hand, MFO swaps their LIVE per-combat combat style
  (CombatController::combatStyle, never the base record) to a stance-matched
  style -- MFO_MeleeStyle (bow starved, avoidThreat 0, so the AI closes and
  swings) or MFO_RangedStyle (keeps distance and shoots). The engine stops
  fighting MFO over which weapon the follower holds. The swap is applied on the
  ENGINE COMBAT THREAD from the UpdateCombat hook (the one callback that hands a
  live controller to every combatant every tick -- an archer's own AI barely
  touches the caster hook), and it reverts with the per-combat controller at
  battle end. Held until battle end or the next equip gambit flips it.
- The stance follows the WINNING GAMBIT, not the follower's class: tell a mage
  to melee and they get the melee stance; hand a mage a bow with a ranged gambit
  and they keep distance and shoot. No per-follower special-casing.
- Debug kill-switch only: set bWeaponStyleControl = 0 in Data/SKSE/Plugins/
  MFO.ini to disable. Not exposed in the MCM -- this is expected behaviour, not
  a setting. Watch "[wstyle]" in MFO.log.

## v1.0.32 -- mage fixes: forced casts actually fire, paced, and potions flow again

- Fix: the v1.0.27 force-on-miss had NEVER fired -- every forced cast died on
  "[pkg] template input 'Spell' is not declared" and fell silently to the
  invisible apply, which is why "cast Firebolt at nearest" still felt ignored.
  Root cause was a package-input name lookup: the live instance's name map is
  non-null but carries input TYPES ("TargetSelector"/"SingleRef"), while the
  input NAMES ("Spell"/"Target") live on the vanilla UseMagic template -- and
  the code only consulted the template when the instance map was MISSING, not
  when the lookup missed. The search now tries the instance, then the template,
  then falls back to the template's statically-known input ids (safe: the slot
  type is still independently verified before anything is written). A miss on
  a cast-at-foe gambit now produces a real, animated "[cast] ... FORCED ... at"
  package cast of the CONFIGURED spell. cast_self stays on the silent fallback
  deliberately: its runtime shape (targType 6 on a QNAM-carrying record) is a
  zero-precedent cell of the class that CTD'd in the field, and it stays barred
  until a dedicated probe clears it.
- Fix: cast cadence is now actually paced by fCastCooldown. The consent hook's
  force-permit had no cooldown consult -- with the spell still in hand it said
  YES on every caster tick, so casts came in bursts (deck: 4 in ~2.2s). The
  cooldown deadline now rides the latch itself (stamped on every cast, read on
  the combat thread under the hook's own lock) and the gambit spell is held
  until due; the deny of competing spells is untouched -- exclusivity and
  pacing are separate dials. One "[consent] ... pacing" line per window.
- Fix: a latched follower's own combat potion-drinking is no longer suppressed.
  The deny hook also fires on the combat POTION caster (CombatMagicCasterRestore
  drives drinking too), so exclusive cast control was silently vetoing his
  emergency drinks. The hook now only ever denies actual SPELLS; potions,
  scrolls, and anything else get the AI's own answer.
- Fix: the AI can no longer slip its own spell in through a condition flicker.
  The suppression latch used to drop on any single service tick where the cast
  condition momentarily didn't hold -- a sub-tick window the combat casters,
  ticking far faster than the scheduler, happily used. Once a cast rule has
  been winning, exclusive control now holds for the combat's duration and
  releases only at combat end, dismissal, or revert/load -- never later.
- New (dev probe, DEFAULT OFF): bProbeCastStyle swaps a latched follower's
  live per-combat style to a new caster-forward MFO_CastStyle record (magic
  branch strongly preferred, melee starved) and restores it on release, to
  measure whether a magic-inclined disposition makes his own AI cast more --
  and mobile -- ahead of the full cast-control work. INI-only, off everywhere
  by default, inert unless armed; "[probe cstyle]" lines carry the data.

## v1.0.31 -- pure casters: no armor looting, and school robes that actually register

- Fix: a magic user no longer loots RATED armor at all -- heavy OR light
  (marth: PURE CASTER). v1.0.29's mage-apparel gate handed any "no school
  match but rated" piece back to the plain by-rating path, which is exactly
  how Marcurio, a correctly detected Destruction user, looted a Dwarven Heavy
  Cuirass and Chitin Heavy Boots. The whole rating path is now skipped for a
  magic user: his loot is clothing/robes (rating 0) only. Armor he ALREADY
  wears is untouched -- nothing strips gear; it simply stops being replaced
  by more armor, and a valid school robe naturally displaces it on that slot
  once one is found (the MEO gem carry-over on that swap is unchanged).
- Fix: school-robe detection now recognises modded/enchanted robes. v1.0.29
  matched only each effect's primary AV against the vanilla school-modifier
  AVs, so LoreRim's re-authored robe enchantments scored 0 and a genuine
  Destruction robe read as junk -- never preferred, which is why Marcurio
  would not switch to his robe and a plain circlet won as "best". The school
  is now the FIRST read that resolves: primary AV; then the MGEF's "Magic
  Skill" (associatedSkill -- the same field the spell side already trusts);
  then the secondary AV on value-modifier archetypes (Value/DualValue/
  PeakValue, where the second AV is a real target); then a keyword naming the
  school on the MGEF, the enchantment, or the armor record itself. Curses
  (Detrimental/Hostile effects) still never read as a boost.
- Fix: no junk picks. A score-0 plain piece must now be a GENUINE upgrade:
  dress a bare clothing slot (head/body/hands/feet) or strictly beat worn
  rating-0 rags on value. It never fills an empty circlet/hair bit (a plain
  circlet is jewellery -- the player's Valuables tier) and never replaces
  real worn gear, so with nothing valid on the corpse the follower keeps
  what he wears.
- New diagnostics: every apparel candidate a magic user evaluates dumps its
  real record data once per base form + target school ("[loot] apparel ...
  primAV/assocSkill/secAV/arch/mag/kw -> school"), so if detection still
  misses a robe the next fix comes from the log, not another guess. Deduped
  session-wide -- no per-tick spam.

## v1.0.30 -- cast control holds through the pause between casts

- Fix: v1.0.28's exclusive cast control had a between-casts leak. The moment the
  gambit's spell fired, the suppression latch was dropped and only came back
  when that follower's next service tick re-asserted it -- and during the cast
  cooldown it never came back at all, so for that whole window (over half a
  second in a full party, the entire cooldown in the worst case) his AI was
  free to slip in its own spell: Chain Lightning between two commanded
  Firebolts. Suppression now persists CONTINUOUSLY for exactly as long as the
  cast gambit keeps winning: the latch holds through the cast and its cooldown
  and releases only when the rule stops winning, combat ends, the follower is
  dismissed, or the game reverts (dismissal and combat end are new explicit
  release points -- a latch must never outlive the fight or the party slot
  that armed it). Nothing but the gambit's spell ever leaves his hands, even
  between the paced casts. Spell choice, cast pacing (fCastCooldown), the
  AI-first grace, the miss detector, force-on-miss, melee fallback when out of
  magicka, and observe/log mode are all unchanged -- this closes the gaps; it
  does not add casts.
- New log, throttled to cast cadence: "[consent] holding exclusive control
  through the cast cooldown" (one line per gambit cast), and the DENIED line's
  dedup now resets each cast so a suppressed own-spell logs once per cooldown
  window -- the deck log shows the gap staying closed without tick-rate spam.

## v1.0.29 -- magic users loot like mages (school robes + a backup dagger)

- New: a magic-user follower -- anyone with at least one enabled cast gambit;
  role stays gambit-driven, never skill-guessed -- now loots SCHOOL-APPROPRIATE
  apparel. Enchanted robes/hoods/gloves/boots that boost his most-cast school
  (weighted across his cast gambits' spells, read from each spell's magic
  effect at loot time -- no catalog regen needed) beat raw armor rating on
  those slots, so a destruction mage finally upgrades into Destruction robes
  the old rating gate could never even see (rating-0 clothing was rejected
  outright). School match is the primary key; among matches the FANCIER piece
  wins (gold value + fortify strength), so with MEO's finer enchanted stock in
  the world the richer robe is the one he takes -- and his socketed gems carry
  over to the new piece through the existing MEO gem transfer. Non-magic slots
  keep the plain rating rules, and a plain piece can never strip worn school
  gear (no loot thrash).
- New: a magic user with no melee role of his own loots ONE one-handed melee
  backup -- daggers only by default -- so when his magicka runs dry the
  vanilla AI has something to draw instead of leaving him swinging fists.
  Stocked in the pack (never equipped over his casting hands), never shed as
  off-role, and never duplicated: one sidearm, not an armory.
- New: two MCM toggles under Logistics, both ON by default: "Magic loadout
  (school gear + backup)" (bMagicLoadout, the master switch) and "Mage backup:
  daggers only" (bMageDaggersOnly; OFF = the best of any one-handed weapon).
  Seeded and self-healed into the MCM store so they bind on existing saves.

## v1.0.28 -- cast gambits take full control (no more casting his own spell too)

- Fix: v1.0.27 made the follower cast the gambit's spell, but he still cast his
  OWN spell alongside it -- an improvement, not the control the premise dictates.
  While a cast gambit owns him, MFO now SUPPRESSES every other spell: the AI is
  denied its own casts (Chain Lightning, favourites) so the gambit's spell is the
  only thing that ever leaves his hands. The AI-first animated path is unchanged
  when he casts the RIGHT spell; only competing casts are cut. Melee fallback when
  he is out of magicka is untouched (that is not a cast), and this only acts while
  MFO is actively driving casting -- observe/log mode still just watches.

## v1.0.27 -- cast gambits cast YOUR spell (hybrid forced cast) + line of sight

- Fix: "cast Firebolt at nearest foe" finally means Firebolt. The old path only
  EQUIPPED the chosen spell and waited ~3 s for the follower's own AI to cast it
  -- and a mage with his own favorites (Marcurio) cast Chain Lightning instead,
  which swapped the equipped spell, forced a re-equip, and RESET the grace clock
  forever; the configured spell was ~never cast. The AI-first window stays (it is
  the mobile, animated path when the follower cooperates), but a MISS -- the AI
  cast a DIFFERENT spell during the grace, or cast nothing by grace end -- now
  forces the configured spell through the field-proven cast package
  (MFO_CastPackage, animated, aimed at the rule's chosen target). New
  "Force the chosen spell" MCM toggle, ON by default.
- New: the cast package's Claim-and-Release pop. Nothing previously ENDED a cast
  package -- it rooted the follower while it owned him. The commanded cast is now
  observed by the [cast] sink and the follower is handed back a moment later
  (backstopped by a 12 s hard timeout); release evicts the alias with the
  non-actor marker (never the player -- the furniture lesson), and the load sweep
  now evicts the cast alias too with the same measured detach readback, so a save
  written mid-cast self-heals on load.
- New: line-of-sight gating. Foe selectors now prefer a foe the follower can
  actually SEE (nearest sighted wins; if every candidate is occluded he still
  engages, as before), and the FORCED cast requires sight -- no more firebolts
  into walls. The raycast runs on the main thread only (the worker reads a
  cached verdict), per the job-thread invariant.

## v1.0.26 -- furniture fix part 2: un-latch the PLAYER from old saves

- Fix: v1.0.25 stopped NEW releases from parking you in the package-carrying
  aliases, but any save written by v1.0.24 or earlier already had you serialized
  INTO them (up to five travel/flee alias packages on the player) -- and the load
  sweep deliberately skipped the player, so the latch survived every load and
  furniture stayed broken even with everything else idle (deck log: marker minted,
  zero evictions all session, still ejected). The load sweep now displaces the
  player with the eviction marker too, and proves the detach with the same
  readback every follower release gets. One load on v1.0.26 cleans the save.

## v1.0.25 -- no more being yanked out of chairs (furniture ejection fix)

- Fix: sitting, mining or smithing could eject you ~once per second. Releasing a
  follower from a loot/retreat excursion works by force-filling the quest's ACTOR
  alias with another ref to displace him -- and that ref was YOU. The alias carries
  a travel/flee package, and forcing the player into a package-carrying alias makes
  the engine pull you out of furniture to run it. Releases now displace with a
  non-actor XMarker (minted once per session): the follower is freed identically,
  but nothing runs a package on a marker, so nothing ejects you.
- Fix: the churn that fired it -- the arm gate measured the CORPSE to the player,
  the release gate measured the FOLLOWER (leash x1.15). A follower parked past the
  leash armed an excursion toward an in-leash corpse, was judged "left leash", and
  released -- every tick. A new excursion no longer arms when the follower is
  already beyond the release margin; arm's-reach grabs are unaffected.

## v1.0.24 -- retreat actually disengages (was firing but not moving)

- Fix: auto-retreat fired but the follower kept fighting in place -- she never
  walked back. A kIgnoreCombat travel package only lets the package run; an actor
  with a live combat target keeps fighting and out-competes it. The retreat now
  calls StopCombat (breaks HER target, not the group) on dispatch and each tick, so
  the travel wins and she actually falls back to you. The [retreat] log now reports
  moved= (her real displacement) instead of only dPlayer, which also closed when
  YOU walked toward her (the false-positive that hid this).

## v1.0.23 -- MCM self-heal runs early (new settings bind on first load)

- Fix: a new MCM setting added by an update (e.g. "Ignore weak potions") could show
  and set -1 -- unbindable -- because MFO seeded the MCM store at kDataLoaded, but
  MCM Helper registers its settings at ITS kDataLoaded, which SKSE dispatched first.
  The self-heal now runs at kInputLoaded (before ANY plugin's kDataLoaded), so the
  key is in the store when MCM Helper reads it and the control binds on the first
  launch -- no game restart needed for the next update's new settings.

## v1.0.22 -- multi-follower loot excursions (P7)

- Up to FOUR followers now walk off to loot at the same time, instead of one at a
  time while the rest waited. A post-fight battlefield reads like a competent party
  sweeping it, not one looter with spectators. The loot quest carries four
  independent excursion slots; each follower claims a free slot and the others keep
  grabbing what is already at arm's reach. Single-follower behaviour is unchanged.

## v1.0.21 -- auto-retreat on by default (combat-sense 3)

- Followers now fall back to your side by default when badly outmatched (low
  confidence -- foe count, level and health feed it) and far from you in combat,
  the confidence leash taken to its conclusion. This is combat-sense 3's default
  rule; the "Auto-retreat when outmatched" MCM toggle is the override -- turn it
  OFF to require an authored fall-back rule instead. Fires once per fight.

## v1.0.20 -- logging efficiency + regression-guard probes

- LOGGING: keep every line but stop flushing per-line. Info now batches to a 1s
  background flush (warn/err still flush at once), cutting per-line disk writes and
  the micro-stalls they caused, with only a ~1s trailing-info risk on a hard CTD.
- [ownprobe]/[potprobe] restored as LEAN regression guards: they fire only on the
  anomaly (a killed corpse read as owned; a follower with alchemy but no drink
  match), so proven features stay watched without log bloat.

## v1.0.19 -- looting competence pass (Fable review P1-P6) + potion rework

- POTIONS: ignore low-power restore potions entirely, loot strongest-first, no cap.
  "Low power" auto-derives from your load order's weakest tier ([potfloor] at
  startup); iMinPotionMag overrides. Applies to all restore types + the buy side.
- ECONOMY (fix): followers no longer sell MEO-socketed weapons (gems were being
  discarded with the sale). Also sell/buy honour the low-power floor and best-first.
- AMMO: same-tier restock finally works -- "restock arrows if < N" was upgrade-only,
  so a low archer never refilled from corpses full of his own arrows.
- DIBS: gold/jewelry you walked past but never took now releases a few seconds after
  you leave, instead of the follower waiting on it forever.
- STEALTH: crouch-walking no longer blocks all looting; a follower only holds off
  while you're actively stealthing (sneaking + weapon drawn or in combat).
- Armor upgrades now pick the best piece, not the first found. Config ini inline
  comments parse correctly. Out of combat a follower now drinks the WEAKEST potion
  that covers the deficit (not its best), and tells you when it hands you an off-role
  weapon. [ownprobe]/[potprobe] diagnostics retired (their questions are settled).

## v1.0.18 -- B back-out reads ImGui's real nav state

- BOARD: B now steps back through ImGui's actual nav layers -- an open picker, a
  focused scrolling child/list, or the menu layer -- and closes the board only at
  the true top, instead of the board's narrower popup-only guess (which exited the
  whole board from the Gambits section). Also logs the full nav state ([bcancel])
  on each B/Esc so any remaining "closes from the wrong level" case is pinned.

## v1.0.17 -- board close: no Tween leak; sturdier B back-out

- BOARD: closing the board no longer pops Skyrim's Tween menu. The board is an
  ImGui overlay with no game menu, so the button PRESS that closed it was leaking
  its release to the game. Now a short input-swallow grace runs from the close
  until that button is released, so nothing leaks.
- BOARD: the B back-out guard now reads ImGui's authoritative open-popup state on
  the render thread (not just our per-frame flag), so B can't misjudge a picker as
  closed and exit the whole board. Added a [bcancel] log line on each B/Esc to
  pin any remaining case.

## v1.0.16 -- B cascade fix, real root cause (single B path)

- BOARD: found why the last two B fixes did nothing. ImGui's Win32 backend polls
  the controller over XInput ITSELF, on the render thread, and feeds physical B as
  its native nav-cancel -- so B reached ImGui TWICE (once from the backend, once
  from MFO's input hook) on two racing threads, and no input-side logic could win.
  Fix: stop adding a second B path. The input hook no longer touches gamepad B
  (the backend's nav-cancel closes the open picker); the board-close is decided on
  the render thread, off that SAME keypress, only when no picker was on screen.
  One signal, one thread -- picker up: B backs out of it; root: B closes the board.
  Keyboard Esc/Tab still forward Escape (the backend does gamepad only). Verified
  by adversarial review.

## v1.0.15 -- B cascade fix, healing-potion stock cap

- BOARD: B now truly cascades. It was closing the whole board even with a list-
  picker open, because the input hook read ImGui's popup state cross-thread (that
  state lives on the render thread) and always saw "nothing open". The render
  thread now publishes the busy state to the input thread, so B closes the open
  picker first and exits the board only at the root. Keyboard Esc shares the fix.
- HEALING STOCK CAP: a follower loots at most N Restore-Health potions (MCM
  "Healing potion stock", default 4), STRONGEST available first -- a small reserve
  of the best rather than a hoard of every weak potion. The loot scan honours it,
  so a stocked follower won't even walk over for another. 0 = no limit; stamina
  and magicka loot as before.
- [ownprobe] diagnostic: when a potion source is skipped as owned, log whether it
  was a dead actor's corpse or an owned container -- to settle whether killed
  corpses are wrongly read as owned vs. legitimately-owned shop/townsfolk loot.

## v1.0.14 -- FFXII-faithful gambit board

The Field Orders board now really copies FFXII's Gambits menu:
- NO DROPDOWNS: highlight a slot, press A, and a large full-height scrolling LIST
  of every condition / action / value / spell opens -- scroll, pick, B backs out.
  Every slider/combo/text widget is gone (the value is a preset list-pick too).
- DENSE FFXII LAYOUT: a tight 7-column table (~11 rows visible) so a full page
  reads like FFXII's, with a party-context bar (name + rank, prev/next) and a
  Combat/Logistics page selector.
- CONTROLLER FLOW: R1 opens (guarded); inside, R1/L1 switch party member, A picks,
  B is a CASCADED back (closes the innermost picker, exits only at the root), the
  View button switches Followers<->Gambits, Y toggles a gambit line on/off. R1 no
  longer closes the board (it's swallowed for party-switch) -- B closes.
- SKINS UNCHANGED: the condition->action two-tone and the picker overlay pull from
  the active skin, so all four (Ebony & Brass / Dwemer Parchment / Soul Cairn /
  Quicksilver) keep their identity. Presentation only -- frozen opcodes and the
  co-save schema are untouched.

## v1.0.13 -- target-relative range, weapon cleanup, MCM self-heal

- RANGE CONDITIONS ARE TARGET-RELATIVE: "foe within/beyond range" now measure the
  distance to the follower's CURRENT combat target, not the nearest foe of that
  range. The any-foe scan let a DISTANT foe satisfy "beyond" while the follower was
  meleeing a CLOSE one, so an equip-ranged (bow) gambit won every tick even in
  melee and MFO fought its own AI for the bow -- the equip re-fire thrash the soak
  showed. Now the within/beyond pair keys off the SAME foe; the board relabels them
  "Foe targeted within/beyond range" (opcodes unchanged, existing gambits adopt it).
- OFF-ROLE WEAPON SHED: a follower carrying a weapon of a role they don't maintain
  (a 2H on a 1H fighter, a crossbow on a bow user) -- default-loadout or a pre-1.0.12
  leftover the game AI kept equipping -- is handed back to the player, one per idle
  tick. Same gambit-driven roles as loot; skips socketed/quest/creature/staff; never
  disarms. [shed] logs each hand-back.
- MCM SELF-HEAL: on load the DLL seeds any missing MCM key into the settings store,
  so a new toggle in an update binds on a player's EXISTING save with no hand-
  editing (retires the per-toggle band-aid; the same gap is queued for MEO/MAO).
- MCM shows the build version (Interface > Debug), stamped from VERSION.

## v1.0.12 -- weapon stability, combat fall-through, and human texture

WEAPON THRASH FIXED (marth: "Erik switches melee weapons for no reason"):
- Loot no longer chooses weapons by SKILL (which force-equipped melee onto a
  bow-user and thrashed with the AI). What a follower maintains is GAMBIT-driven:
  equip-melee -> best melee skill class; equip-ranged -> a bow/crossbow by ammo;
  neither -> upgrade the role they already wield, in place. Loot STOCKS cross-role
  weapons; only equip gambits move them into the hand (Fable-reviewed: arsenal-wide
  baseline, StripCorpse drains a corpse fully).
- The economy NEVER sells a socketed weapon (an unworn gemmed spare was the leak).

COMBAT FALL-THROUGH (D1, the big correctness fix; Opus-reviewed):
- The combat table now falls through a rule it CANNOT act on (unaffordable cast,
  empty potion, satisfied equip, cast cooldown, 2H debounce) to the rules below,
  instead of that top rule silently shadowing everything for the rest of the fight.
  A follower no longer dies holding a heal he can't afford; spellsword/hybrid lists
  are buildable. Still one gambit action per tick. Satisfied-equip hand-claim stops
  melee<->ranged thrash; spell loans survive fall-through.

FLAIR (feel more human / FFXII): per-follower temperament so the party isn't a
metronome; a brief "ready beat" on entering combat; a weapon-ready flourish on
equip; "sizing up" hesitation before switching targets; the board pulses a rule
as it fires + condition->action coloring; follower names in the log; a rapport
rank-up toast.

## v1.0.11 -- brawl gate + combat sense + loot fix (Opus-reviewed)

- BRAWL GATE (#34): a follower no longer attacks a foe it is not actually hostile
  to. Eval::PickFoe skips any combat-group member failing IsHostileToActor, so a
  tavern brawl or the Companions' proving fight (Vilkas) can't be turned into a
  real assault -- one gate covers attack / power-attack / cast-target. Logs
  [brawl] "held fire" only when a non-hostile opponent was the sole candidate.
- COMBAT SENSE (#23): foe count now folds into the confidence leash -- being
  MOBBED reads less safe than a duel. A five-foe pack drops a full-health follower
  under the auto-retreat floor; a two-foe fight matches the old value, so existing
  leash/chase tuning is unchanged at the common case. New shared CombatSense::
  FoeCount; [sense] tuning log.
- LOOT EFFICIENCY (#30): a follower now grabs loot already at its own feet even
  when it sits a hair past the player-leash bubble -- the "found the potion right
  in front of him only after several runs" report. The leash bounds travel, not
  what he picks up where he stands.
- CURE CATALOG (#35, data layer only): the Synthesis patcher classifies cure-
  poison / cure-disease potions (CurePoison/CureDisease archetypes) and the DLL
  recognises them, so they are not mis-sold. The "drink a cure" gambit is
  deferred until requested. Needs a Synthesis re-run to populate.
- Auto-retreat transition logs raised to info so a field test can see them.
- FIX (Opus review): a combat-group read-lock was nested inside itself via the
  new foe-count read (PickFoe -> ChaseRadius -> FoeCount), which could deadlock
  the game mid-combat if the engine took the write lock between the two reads.
  The chase cap is now computed once before the lock. Caught before shipping.

## v1.0.10 -- release-readiness fixes (Fable RC review)

A Fable "is this good enough to ship to the world?" pass judged the ENGINE ready
but the SHIPPING SURFACE not; the seven blockers it named are fixed here.
- LICENSING (RC#1): THIRD-PARTY-NOTICES.md now enumerates the real statically-linked
  set (CommonLibSSE-NG, Dear ImGui, nlohmann/json, spdlog, fmt -- all MIT) instead of
  leaving transitive deps "OUTSTANDING"; added OFL.txt (SIL OFL 1.1 + per-font copyright)
  for the two board fonts, Cinzel and EB Garamond, which ship in the repo.
- DOCS (RC#2): README gains Requirements + Installation -- SKSE, Address Library, SkyUI +
  MCM Helper >=9, po3 (economy only), Synthesis (catalog); SE/AE only, VR unsupported,
  act.flee is AE-only.
- ECONOMY GATE (RC#3): the merchant scan now runs only when bEconomy is ON *and*
  po3_papyrusextender.dll is loaded (logged once if absent), instead of an always-on
  constexpr probe.
- AUTO-RETREAT (RC#4): the retreat probe is promoted to a real, opt-in leash behavior
  behind a new **bAutoRetreat** toggle (default OFF): a badly-outmatched follower far from
  you in combat falls back to your side. Per-tick diagnostic spam cut to transition-only
  debug lines; the combat-controller observation block removed.
- WAIT IN LOGISTICS (RC#5): "Wait" is now selectable as a logistics action (a deliberate
  no-op that gates lower rules), matching combat.
- PLAYER-HP HEAL (RC#6): a "Player HP% below -> Cast on target" rule now targets the PLAYER,
  not the casting follower -- the condition's subject was being dropped. Gated to the
  cast-on-target action so "Player HP% below -> Attack" can't turn a follower on you.
- SE/AE HONESTY: README + the new toggle's help now state that the walk-to-it behaviours
  (walk-to-loot, Flee, Auto-retreat) are Anniversary-Edition-only; they no-op on SE 1.5.97.
  Combat gambits, restocking, looting, and the economy work on both.

## v1.0.9 -- Fable review fixes for the gambit-v2 tags (#35)

Two adversarial Fable passes over v1.0.1-v1.0.8 found real bugs; all fixed:
LOOT: keep-set now protects the best of EACH weapon class (1H/2H/bow/crossbow/staff)
separately -- merging them let a junk greatsword/crossbow win the keep and the real
weapon get SOLD (staves were also unprotected). LootAmmo peek now agrees with the take
(was walking followers back to corpses holding only shed junk, forever). TableHasAction
respects the enabled flag (a toggled-OFF equip gambit no longer loots). The equip-melee
loot arm is MELEE-ONLY (was handing archers bolt-less crossbows). Legacy COMBAT "equip
torch" rules migrate to logistics on load (would otherwise block every lower combat rule).
COMBAT: power-attack returns Fired only if the anim graph accepts it + requires a drawn
melee weapon (no more passive burned windows); "foe attacking me: melee" excludes staff
foes. REVERTED keep-distance / flee-from-foe (1.0.8): KeepOffsetFromActor's offset is in
the FOE's frame, so "away from foe" is inexpressible -- it flanked instead. Needs a real
travel-away package (deferred). Flee-to-player + power-attack (experimental) remain.

## v1.0.8 -- gambit v2 (4/n): keep-distance + flee-from-foe [EXPERIMENTAL] (#35)

Two new combat actions via Actor.KeepOffsetFromActor (Papyrus dispatch): "Keep distance"
(hold ~450u behind the selected foe) and "Flee from foe" (~1500u, run). A per-follower
latch releases the offset (ClearKeepOffsetFromActor) the instant the follower picks any
other action or leaves combat (Logistics tick), and on revert. EXPERIMENTAL: whether the
combat controller honours the movement override for an AI follower is unproven -- field-verify.
Distinct from 1.0.6 "Flee to player" (retreat package).

## v1.0.7 -- gambit v2 (3/n): arrow quality-trade + torch to logistics (#35)

- ARROW TRADE (#3): each arrow loot now TRADES worst-for-better, count-neutral -- takes
  up to the number of better arrows available off a body, best-first, and sheds an equal
  count of the follower's worst (given back to the body), never shedding one as good as
  what it took. Empty-handed = clean restock; a junk stack gets upgraded in place.
- TORCH -> LOGISTICS (#4): "Equip torch" moved out of the combat table (never needed in
  combat) into logistics; added "In an interior"/"At night" to the logistics conditions
  to pair it. Combat executor no longer dispatches it.

## v1.0.6 -- gambit v2 (2/n): flee + power-attack combat actions [EXPERIMENTAL] (#35)

- FLEE (act.flee): disengage by reusing the retreat package (travel to player under
  kIgnoreCombat). Solid reuse of proven machinery.
- POWER ATTACK (act.power_attack): EXPERIMENTAL -- no engine verb exists, so it aims
  via the latch then fires the standing power-attack anim event; logs whether the
  graph accepted it. Field-verify.

DROPPED from the batch: in-combat potion looting (breaks combat). DEFERRED:
keep-distance (needs its own travel-away package, like retreat); arrow-at-quota trade;
lockpick-doors (needs a verified unlock call). Foe melee/ranged conditions shipped in 1.0.5.

## v1.0.5 -- gambit v2 (1/n): foe-attacking-me melee/ranged split (#35)

New combat conditions "Foe attacking me: melee" and "Foe attacking me: ranged" --
foe targeting the follower, filtered by weapon (steel vs bow/xbow), reusing the
existing FoeTargets/FoeIsRanged/FoeIsCaster helpers. First slice of the vocab-v2 batch.

## v1.0.4 -- keep both weapons of a hybrid (bow+melee) follower (#35)

A follower who uses both a bow and a melee weapon only has one WORN at a time, so
the sheathed other read as unworn junk and got sold. The sell list now protects the
BEST weapon of EACH class (melee + ranged); only worse duplicates are sold.

## v1.0.3 -- equip-melee followers loot a melee weapon regardless of skill (#35)

A follower whose table has an "equip melee" gambit now loots a best-class melee
weapon even at poor skill / empty-handed (they use it occasionally), mirroring the
existing equip-ranged override.

## v1.0.2 -- no shields for two-hander/bow followers

Farkas (two-handed) looted a shield -- useless with no free off-hand. Armor loot
now skips shield-slot gear when the follower's best weapon class is two-handed or
ranged.

## v1.0.1 -- loot armour by class skill (#21 follow-up)

Auri (Heavy Armor skill 5) kept looting heavy plate: ArmorIsBetter compared raw
armour rating only, ignoring class. Now gated by ArmorClassSuits -- a follower takes
heavy armour only when STRICTLY more heavy- than light-skilled, and light whenever
at least as light-skilled; ties and casters fall to light, never heavy. Clothing
(rating 0) unaffected.

## v1.0.0 -- Follower economy + magnum-opus milestone (#21)

First 1.0. Followers now run a full autonomous ECONOMY at merchants -- sell their
unworn junk (highest-value first, capped at the vendor's barter gold) and buy the
supplies they're short on (best affordable, up to the number needed, from the
vendor's ACTUAL enumerated stock) -- via a native<->Papyrus bridge that keeps the
crash-prone merchant reads out of C++ (Fable's ECON_PAPYRUS_PLAN, phases 0-4).
Gated by the bEconomy MCM toggle (off = dry-run plan in the log).

Hardened for release: a full save-safety audit (revert now clears the pump queue,
MEO gem map, probe handles, trade orders) and an adversarial Fable audit of the
bridge -- econ scan moved to the worker thread (no inventory race), a per-chest
in-flight guard (no gold duplication with two followers at one vendor), cross-save
token guard, live-count clamps on every transaction line, and stale-order reaping.

Builds on everything since 0.8.x: loot-travel efficiency (sticky-unreachable churn
fixed), invisible-weapon heal, no-looting-in-player-homes, the MCM economy toggle,
and the Papyrus compile pipeline (Source/ + tools/compile.sh).

## v0.8.49 -- Fable audit fixes (1.0 RC hardening, #21)

Adversarial Fable audit of the econ bridge + save-safety. All findings fixed:
- CRIT #1/#4: econ scan runs on the WORKER now (direct call, not MainThread::Post),
  so follower inventory + g_travel reads share the thread with the loot/heal/loadout
  mutations instead of racing them (the Actor.cpp:445 CTD class). Transaction stays
  in Papyrus (VM); dispatch is worker->VM like DispatchActivate.
- MAJOR #2: per-chest in-flight guard -- two followers can no longer hold live orders
  on one vendor chest (was minting gold: both read the same barter gold, chest paid twice).
- MAJOR #3: g_nextToken jumps +1e6 on revert so a save-suspended RunTrade token can
  never collide with a reissued one (resumes -> no chest -> aborts safe).
- #5/#6: sell/buy clamp to the LIVE follower count/purse at execution (no paying for
  undelivered goods, no delivering unpaid goods if state shifted since dispatch).
- #7: econ cadence clocks hoisted to namespace scope + cleared on revert.
- #8: trade cooldown burned only on a CONFIRMED dispatch; stale orders reaped at 30 s
  (so a leaked order cannot block a chest forever via the new guard).
- #10: [econ] log reports the ORIGINAL purse, not the post-PlanBuy remainder.

## v0.8.48 -- save-safety audit fixes

Audit found three mutable states that outlived a revert (neither serialized nor
cleared), each able to act on a REUSED FormID/handle in the next session:
- MEOBridge g_pending (pending gem-move map) -> new MEOBridge::ClearTransientState.
- MainThread pump queue -> new MainThread::Clear (drops queued closures whose
  captured handles would re-resolve against the next session).
- Probe watch handles -> Probe::ReleaseAll now runs on revert too (was kPostLoadGame only).
All three wired into ResetAllState. Quest alias fills were already covered by
Packages::ReleaseAll on every lifecycle edge.

## v0.8.47 -- econ Phase 4 hardening + save-safety (#21)

- POTIONS: PlanBuy classifies stock with Logistics::PotionRestores (catalog +
  heuristic), the SAME classifier the follower counts/drinks with -- catalog-only
  left an alchemist's potions unbought despite a real need (field: Arcadia BUY 0).
- ANTI-THRASH: per-follower trade cooldown (20s) + one trade per scan, so a
  purchase settles before the need is re-evaluated -- stops the same-scan double-buy
  across two nearby vendors (field: Erik +17 @ Ysolda AND +22 @ Adrianne, one second).
- DON'T-TRADE-MID-LOOT: a follower walking to loot skips trading until the excursion ends.
- SAVE-SAFETY: TradeBridge::ClearTransientState now runs on revert (pending orders
  hold session-only handles; a resumed RunTrade with a dead token aborts safe). The
  BUY loop now pays PER-ITEM (move+pay together, token-free GetFormValue) so a save
  landing mid-loop leaves a consistent partial trade, never free items.

## v0.8.46 -- econ: buy potions via the follower's own classifier (#21)

At an alchemist a follower with a potion NEED bought 0 (field: Arcadia). PlanBuy
classified stock catalog-only, but the follower COUNTS/drinks potions with the
runtime classifier (catalog-first + archetype heuristic) -- so a potion the catalog
didn't know was invisible to buying yet counted toward the need. PlanBuy now uses
Logistics::PotionRestores (the same one), so buy matches count. Arrows unaffected.

## v0.8.45 -- econ Phase 3: follower BUYING via vendor-stock enumeration (#21)

Buy the supplies a follower is short on. Native names the NEED (which supply gambit
is below N, by how much); MFO_Trade ENUMERATES the vendor's actual stock (po3
AddAllItemsToArray -- guessing candidates gave stock=0 everywhere) and hands it to
TradeBridge::PlanBuy, which classifies it via the catalog and picks the BEST
affordable, up to the number needed, bounded by the purse (marth's rule). Papyrus
executes: goods chest->follower, gold follower->chest. Gated by bEconomy (off = dry
run that still enumerates+plans, so the new read is proven before any purchase).

## v0.8.44 -- econ Phase 2: follower SELL at merchants (#21)

Bridge proven (Phase 1, crash-free Papyrus read), so wire the SELL transaction.
Native builds the follower's sell list (unworn, un-excluded, VEND-tradeable gear,
highest-value first) and dispatches it; MFO_Trade reads the chest's barter gold and
sells down that list capped at the chest gold (RemoveItem follower->chest, pay the
follower, deduct chest gold), reporting what moved. New MCM toggle bEconomy (default
OFF = dry-run log only; ON = actually trade). BUY (Phase 3) needs vendor-stock
enumeration (candidate-guessing gave stock=0 everywhere) -- next.

## v0.8.43 -- FIX Phase 1 CTD: count purse from GetInventory, not GetGoldAmount (#21)

Breadcrumb [bc]3 + the PDB pinned the v0.8.40/0.8.42 vendor crash to
RE::Actor::GetGoldAmount(bool) (Actor.cpp:445) -- a null-deref reading the
follower purse, on the InventoryChanges the worker tick may be mutating. The
GetInventory snapshot in the same frame is safe, so the probe now sums Gold001
(0x0000000F) straight from it and never calls GetGoldAmount. Probe stays enabled;
[bc] breadcrumbs kept for one verify run.

## v0.8.42 -- DIAGNOSTIC: re-enable econ probe with PDB + breadcrumbs (#21)

Pins the v0.8.40 vendor CTD. CMake now emits MFO.pdb in Release (CI uploads it), and
EconomyProbe logs [bc] 1..6 before each risky step (resolve, sell GetInventory,
purse GetGoldAmount, buy-walk, dispatch). The last [bc] before a crash = the exact
faulting step; the PDB symbolizes the address. Expected to crash once more, on
purpose, to capture that.

## v0.8.41 -- HOTFIX: disable econ probe (Phase 1 CTD)

v0.8.40 re-enabled the econ probe; it CTD'd on the main thread near a real vendor
(Bannered Mare), a null-deref in the probe build path (NOT the Papyrus dispatch --
Phase 0 proved that). Disabled again so testing can continue. Phase 1 returns once
the crash is pinned with symbols (CI PDB fix pending) + per-step breadcrumbs.

# MFO — Changelog

Versions are immutable once released. Bump `VERSION` for every build that
reaches the game.

## v0.8.40 — econ bridge Phase 1: read-only merchant probe (#21)

The crash gate. Native resolves the vendor (teammate skip, VisitFactions
IsVendor+OffersServices, GetContainer gate) and builds the plan from SAFE reads --
the follower's own inventory (WOULD SELL) and catalog-walked buy candidates ranked
by value (WOULD BUY) -- then dispatches a token'd order to MFO_Trade. The script
does the crash-prone reads in Papyrus (`chest.GetItemCount(Gold001)` + per-candidate
stock, the barter-safe path C.O.I.N. proves) and reports back; native logs the full
`[econprobe] WOULD SELL / WOULD BUY` plan. ZERO transactions. This is the read the
old native probe kept CTD-ing on (disabled since v0.8.31) -- now crash-free.
TradeBridge gains the pull accessors (GetVendorChest/Actor/BuyCandidates/ProbeOnly)
+ ReportProbe; the Phase 0 NativePing self-test is retired.

## v0.8.39 — econ bridge Phase 0: the native↔Papyrus round trip (#21)

First increment of Fable's ECON_PAPYRUS_PLAN. The merchant read/transaction can't
live in C++ (native GetInventory on an unpopulated merchant chest CTDs), so it moves
to Papyrus; native keeps the decision. This build stands up and PROVES the bridge
before any merchant is touched:

- ESP: new `MFO_TradeQuest` (0x80E) carrying `MFO_Trade` (VMAD), start-game-enabled
  + SEQ (audit_esp updated; frozen-id table + SEQ_EXPECTED).
- Papyrus pipeline (NEW): `Source/Scripts/MFO_Trade.psc` + `Source/Stubs/` (mirrors
  MAO/MEO) + `tools/compile.sh` (Proton-wine + Nemesis PapyrusCompiler). release.sh
  recompiles + ships `MFO_Trade.pex` every build.
- Native `TradeBridge.{h,cpp}`: registers MFO_Trade's Papyrus natives (Phase 0:
  `NativePing`), and a self-test that dispatches `RunTrade(1)` once the quest is
  running. `Papyrus::DispatchTradeRun` adds custom-class dispatch. Forms resolves
  `g_tradeQuest`.
- Proof = the log shows `[trade] registered …`, `[trade] self-test: dispatching …`,
  then `[trade] NativePing token=1 -- bridge round-trip OK`. Phases 1–4 (read-only
  probe → sell → buy → harden) build on this.

## v0.8.38 — no looting in player homes (default; MCM toggle)

marth: followers shouldn't rifle your own house. Looting is now suppressed in any
cell whose LOCATION carries vanilla LocTypePlayerHouse (0x01CB85, Skyrim.esm) --
bought houses, Hearthfire builds, and the home mods that set it. New MCM toggle
`bLootInPlayerHomes` (Logistics page), DEFAULT OFF; turn it on to loot there too.
Only LOOTING is gated -- drinking/supply still run. An excursion already underway
is cleared ("player home") the moment you step inside. Gate lives in LootNearby's
early-out + the excursion driver's hard-interrupt block.

## v0.8.37 — stall strikes survive the reassess (kill the unreachable-body churn)

The [lootskip] capture proved the real efficiency killer: a follower burned whole
excursions re-walking to a GEOMETRICALLY UNREACHABLE body (deck 0002CFBF, navdist=148
< off-navmesh gate 300, so the gate passed it; the navmesh path ends ~148u short and
`dist` froze at 975/979 every walk). The 2-strike sticky blocklist should have caught
it, but the idle reassess called `g_stallStrikes.clear()` every ~15s, resetting the
strike to 0 before the 2nd stall could promote it -> re-picked forever.

- Stall strikes now SURVIVE the reassess, so a body that stalls once per excursion
  accumulates to the 2nd strike and goes sticky (5 min, won't re-pick).
- A body the follower actually REACHES has its strike cleared on arrival, so a merely
  transient block (boxed in by an actor/door) never falsely accumulates to sticky.

Known-remaining (measuring next): a reachable-but-empty body ("arrived, nothing to
take", deck 0007F61D) still re-picked after the reassess; and the single global loot
alias serialises the two followers (aliasBusy in [lootskip]) -- a 2nd alias is an ESP
change, tracked separately.

## v0.8.36 — loot skip-reason diagnostic ([lootskip])

marth: "not sure why loot efficiency was so bad" -- Eric grabbed a potion right in
front of him only after several runs. The scan log shows candidates were eligible
but the per-candidate SKIP reasons in the act loop (player-bubble convergence yield,
fail/sticky blocklist, single-alias busy, player-considering, off-navmesh) were
silent. Added a rate-limited `[lootskip]` line: when a tick collects candidates but
loots nothing, it recomputes and logs WHY the CLOSEST eligible body was passed over.
Diagnostic only -- no behaviour change. Prime suspect: the 256u convergence bubble
(a body near YOU is deferred to you) plus the single global loot alias serialising
the two followers.

## v0.8.35 — heal evicts the creature weapon (stop the re-wield loop)

The v0.8.34 heal only *unequipped* the non-playable creature weapon, leaving it in
the follower's pack. The engine re-wielded it as "best weapon" within seconds, so
the 5 s poll churned forever (field: the same Dwarven Sphere Crossbow re-healed
3 min apart). The heal now RemoveItem()s every copy to the player (kStoreInContainer),
so nothing can re-select it.

## v0.8.34 — heal + block wrongly-looted non-playable (creature) weapons

Confirmed via MEO's [wdiag] logging: MFO looted a "Dwarven Sphere Crossbow" (an
automaton's built-in weapon, no humanoid model) off a corpse and equipped it on
both followers -> invisible weapon that still fires (ExtraWorn intact; the MEO
two-hander slot bug was a separate, now-fixed issue).

- Catalog (patcher) now excludes every NON-PLAYABLE weapon, so LootEquipment's
  IsExcluded skip stops looting creature/automaton gear + Bound weapons going
  forward (re-run Synthesis to regenerate; 473 -> 723 excluded).
- HEAL (DLL): a follower found wielding an excluded weapon has it swapped for their
  best carried PLAYABLE weapon (or unequipped if they carry none). Rate-limited 5 s,
  no-op once healed. Reuses Catalog::IsExcluded, so it needs the regenerated catalog.
Both the loot filter and the heal ALSO use a direct runtime non-playable check
(record-header flag bit 2, GetFormFlags), so this works off the DLL alone -- no
catalog regen required. Reload and the crossbows swap out and never return.

## v0.8.32 — fix: followers churning unreachable loot legs forever

Root cause of both followers looping unreachable corpses (v0.8.31, pathSpeed=0,
navdist<<dist, act.loot_equipment re-firing for minutes): the excursion RETARGET
resets the no-progress tracker (progressAt) every tick, and it re-picked the
closest walkable ref every ~2-4 s -- faster than the 7 s stall timer -- so an
unreachable leg NEVER accumulated the stall that sticky-blocklists it (the v0.8.30
fix could never trigger). The follower ping-ponged legs indefinitely.

- Excursion now COMMITS to the current leg: while already walking to a still-valid,
  not-yet-stalled target, it does not retarget. The Walking-phase arrival/stall
  logic finishes the leg first, THEN the scan picks the next. So a stall finally
  accumulates -> 2 strikes -> sticky-unreachable -> excluded -> the loop converges
  and he returns to the player. Arm's-reach grabs and reachable multi-corpse
  looting are unchanged.

Note: does not by itself address the invisible-drawn-weapon report (under separate
investigation); it stops the loot-travel churn that co-occurred with it.

## v0.8.31 — econprobe disabled again: the vendor CTD is NOT a thread race

The v0.8.29 pump ran the econprobe on the MAIN thread and it STILL crashed
(crash-2026-07-31-15-49-45, vendor Ma'dran) with the identical signature -- a
null cast-deref inside chest->GetInventory() on the merchant's chest (RSI=Chest).
Decisive: the fault is native GetInventory on a persistent merchant container
whose InventoryChanges no barter menu has populated -- it faults on ANY thread,
not a race. GetContainer() passes; the fault is deeper.

- Econprobe disabled (kept compiled behind if(false)). The main-thread pump
  (§0.37) stays -- it's still correct + needed for the barter TRANSACTION
  (mutations must run on main), just not sufficient for the READ.
- Real barter needs a SAFE merchant-stock read (Papyrus, as the game's own
  barter menu uses; or force-initialising the chest inventory) -- not native
  GetInventory on an unpopulated merchant chest. Tracked under #21.
No gameplay change; loose-item/retreat probes unaffected.

## v0.8.30 — fix: frozen follower looping on an unreachable loot item

Erik froze ping-ponging between two arrows he could never reach (00020169
navdist=18 vs dist=630, and 0002016A) -- an off-navmesh item gives a short
navmesh path that ends far from the item, so he walks "there", can't close the
last gap, and the idle reassess (which wipes the 25 s block so bodies reachable
AFTER moving get re-tried) kept resurrecting them into an infinite re-attempt
loop. Not related to the v0.8.29 pump; pre-existing loot-travel.

- A 2nd STALL on the same ref now promotes it to a STICKY unreachable set: a
  5-minute cooldown the idle reassess does NOT clear. One stall is still just
  transient (a momentary block); two is a verdict. Logs "[loot] .. STICKY-
  unreachable". Sticky set clears on revert/load. Both the walk-stall and the
  two dispatch-time off-navmesh guards feed it.
Loot now gives up on a genuinely unreachable item and moves on / returns to you.

## v0.8.29 — main-thread pump (MFO::MainThread) + econprobe on it

The foundational fix behind #21. MFO's tick runs on a BSJobs job worker and
AddTask stays on a worker in this runtime, so reading a live merchant's
inventory raced the main thread and CTD'd (v0.8.26-28). New primitive:
MFO::MainThread::Post(fn) runs fn on the MAIN thread, drained from a vfunc hook
on the player's per-frame Update (VTABLE_PlayerCharacter[0] idx 0x0AD, verified
against pinned CommonLibSSE-NG; VR-refused since the index shifts). Reusable —
retreat actuation and future engine work can hop to main through it.

- The economy probe is re-enabled, now Posted to the main thread (follower by
  handle, gambits by-value copy). Still LOG-ONLY — this run VALIDATES that
  pump-side vendor reads (Ulfberth/Warmaidens, the exact CTD case) don't crash,
  before any real barter is built. Watch for [mainthread] first drain ... pump
  is live, then clean [econprobe] lines near a real merchant.
No transactions yet; loose-item/retreat probes unchanged.

## v0.8.28 — hotfix: disable economy probe (off-thread vendor read CTD)

The v0.8.27 teammate skip removed one trigger but not the real bug: the
[econprobe] read a LIVE vendor's inventory (chest->GetInventory /
vendor->GetGoldAmount) from the logistics JOB WORKER, racing the main thread
that manages the merchant. A form cast returns null mid-race and is dereferenced
-> CTD on REAL vendors too (Ulfberth/Warmaidens, crash-2026-07-31-13-12-39).
Corpse loot is safe on the worker (static inventory); a live merchant is not.

- EconomyProbe is DISABLED (kept compiled behind if(false), so it can't bitrot).
  #21's real barter/buy system must read vendor inventory on the MAIN thread --
  and SKSE AddTask does not reach main in this runtime, so it needs a genuine
  main-thread mechanism, not AddTask.
- Loose-item [acquire] and retreat [retreat] probes are UNAFFECTED (they touch
  static world refs / packages, never a live actor's inventory off-thread) and
  keep instrumenting.

## v0.8.27 — hotfix: economy probe CTD on a follower-as-vendor

The [econprobe] treated any nearby actor whose faction passes IsVendor() &&
OffersServices() as a merchant. A fellow FOLLOWER (Auri) carries a stray vendor
faction but a malformed merchant container; reading it null-derefs a form cast
inside GetInventory and crashes to desktop (EXCEPTION_ACCESS_VIOLATION at
MFO.dll, on the logistics job worker). Reproducible.

- Vendor candidates now skip teammates (a follower is never a merchant; teammates
  trade through the player, not each other) — kills the exact trigger.
- The merchant-chest read is gated on chest->GetContainer() (a real container
  REFR) before walking it — defense-in-depth so any other stray-vendor actor
  can't recur the same fault.
Instrument-only still; no transactions. Loose-item and retreat probes unchanged.

## v0.8.26 — three engine probes (loose-item, retreat, economy) — instrument-only

Log-only builds to reveal three mechanisms in natural play (no deliberate test):
- [acquire] LOOSE-ITEM: an archer's spent arrows / dropped gold now route through
  the proven travel walk, then a VM-dispatched ObjectReference.Activate picks them
  up (engine does it — no PickUpObject on our thread). Logs dispatch + a next-tick
  ref-gone/inventory-delta readback.
- [retreat] RETREAT: at low confidence + in combat + >400u from you, a new alias
  Travel package (kIgnoreCombat, static-60 + eviction) tries to pull the follower
  back to you. Logs whether the travel package holds DURING combat vs the combat
  controller keeping locomotion; a [retreat-b] rider logs the vanilla isFleeing state.
- [econprobe] ECONOMY: near a merchant, logs what MFO WOULD sell (unworn gear +
  jewellery the vendor buys) and WOULD buy (logistics quotas), plus the vendor's
  gold. Zero transactions — validates detection/filter before any gold moves.
No behaviour change beyond the loose-item pickup; probes come out once read.

## v0.8.25 — new gambit vocabulary + loot depth (big-patch batch 1)

New COMBAT conditions: Foe is a spellcaster / is ranged / is weaker than me / is
blocking / is fleeing (all read the foe's live state — e.g. "Foe is fleeing ->
Wait" so he stops chasing runners; "Foe is a spellcaster -> Cast at foe" to shut
down mages). New LOGISTICS: Loot soul gems, Loot lockpicks, and a "Carry weight %
above N" condition (guard rule: above 90% -> Wait, so he doesn't loot himself
immobile). Ammo restock now takes the HIGHER-QUALITY arrows/bolts first to fill a
quota. And the jewellery slot heuristic now supplements the catalog instead of
going dead once a catalog loads (catches mod rings added after the last patcher
run). Soul gems are catalogued (quest gems like Azura's Star excluded).

## v0.8.24 — combat sense 1: confidence-scaled chase range (dev)

Erik no longer charges 20 Falmer solo to reach the weakest one. Confidence now
feeds combat (the second consumer this system was always meant to have): a
follower only auto-picks a foe within a confidence-scaled CHASE RADIUS of
himself. Hurt or mobbed -> it shrinks, he fights only what's on top of him and
holds near you; healthy and safe -> it widens, he ranges. The "attack the
weakest foe" order can't march him across a pack anymore. Floor stays above
melee so he always defends against an adjacent foe; your own explicit
within/beyond-range gambits bypass it. Tunable via fChaseMin/fChaseMax.

(First of the combat-sense stack; foe-count-into-confidence and a real retreat
follow.)

## v0.8.23 — gem transfer only replaces same-role gear

Fix: a follower with a fire enchant on his MELEE weapon lost it to a looted BOW.
The gem-transfer captured the equipped weapon as the "replaced" item for ANY
weapon pickup, so a new ranged weapon stole the melee's gems even though it
replaces nothing. Now gems move only between items of the same role — melee to
melee, bow to bow, crossbow to crossbow (armour was already slot-matched). A new
bow no longer touches the sword's enchant.

## v0.8.22 — ranged looting tells bows from crossbows

The equip-ranged loot pickup no longer lumps bows and crossbows together (they
feed different ammo — a crossbow-user handed a bow is stuck with no bolts). The
follower now loots only the kind it can actually use: whichever ranged weapon it
already carries (upgrading the better of the two), else the kind matching the
ammo it holds (arrows -> bow, bolts -> crossbow), else a bow by default.

## v0.8.21 — carry your gems onto looted gear (MEO API) + loot bows for archers

- **Gem transfer on gear swap (#17).** When a follower upgrades to looted gear,
  their socketed enchant gems come with them — via MEO's new inter-plugin API
  (`IMEO::MoveGems`). Captured at the swap and fired once the new piece is worn
  (so MEO can mint its instance id); gems that don't fit return to the shared
  pouch with their XP intact. Fully optional — no-ops cleanly if MEO isn't in the
  load order. Also exposes a "with my gems" preview (`PreviewWithGems`) for the
  board to show compared stats with the current gems simulated.
- **Ranged-weapon looting (marth).** A follower whose gambits include "equip
  ranged" now loots a bow/crossbow off corpses even when they aren't already
  wielding a weapon or their melee class isn't ranged — so an archer actually
  gets a bow to use. Covers vanilla and modded ranged weapons.

## v0.8.20 — loot jewellery + a walk/jog/run gait toggle

Two features built in parallel:

- **Loot jewellery (#11).** A new "Loot jewellery" logistics gambit. Jewellery is
  identified from the catalog (`mfo_items.json`), falling back to the amulet/ring
  slot with no armour rating. It rides the same valuables dibs as gold (you get
  first pick), and never touches catalogued never-loot items.
- **Walk/jog/run gait (#16).** A "Walk-to-loot gait" MCM option on the Behaviour
  Layer page. It sets the walk-to-loot excursion's gait by writing the travel
  package's preferred-speed byte (the mechanism ENGINE_NOTES §0.35 measured) —
  no hook, re-applied from config each session. Default Run, matching the shipped
  package, so nothing changes unless you pick Walk or Jog. Normal following is
  unaffected (that's the follower framework's own package, which MFO never edits).

## v0.8.19 — arrows, solved: the catalog fixes them too

The arrow probe finally caught it: a corpse the follower was standing on held
`Iron Arrow x11, Steel Arrow x2, Ancient Nord Arrow x3` — and every one reported
`IsBolt()==true`. So it was never adjacent-cell scanning; the engine's own
`IsBolt()` mislabels vanilla arrows as bolts here, and the follower's "loot
arrows" gambit (which wants non-bolts) rejected all of them. The `bolts=1` we
kept seeing was him picking these arrows up *as bolts*.

The catalog already had them right — arrows are `arrow`. So MFO now classifies
ammo through the catalog (arrow vs bolt from the real record), falling back to
`IsBolt()` only for uncatalogued ammo. He'll finally strip a body's arrows.

Needs the catalog present (run MFO.Synthesis); with v0.8.18's potion fix in the
same build, one relaunch should give you both drinking AND arrow looting.

## v0.8.18 — the item catalog: read the load order, stop guessing

The follower kept refusing to drink health potions he was carrying: the runtime
classifier keys off MGEF archetypes, and Requiem builds its restore potions
differently, so they read as "not a potion." The fix is architectural, borrowed
from MAO: a **Synthesis patcher** (`installer/MFO.Synthesis`) reads the ACTUAL
records of your whole load order and writes `Data/SKSE/Plugins/MFO/mfo_items.json`
— which potions restore health/stamina/magicka (by effect flags, not archetype,
so Requiem/CACO work), arrows vs bolts, jewellery, and a "never loot" list of
quest items, artifacts/unique enchantments, and scripted items.

This build is the DLL half: MFO now loads that catalog at startup and trusts it —
`PotionRestores` becomes a catalog lookup (falling back to the old heuristic when
no catalog is present, so the mod still works standalone), and the follower skips
catalogued "never loot" gear. Run the MFO.Synthesis patcher (added in Synthesis
from this repo) after installing MFO and after any load-order change.

(Arrows are unaffected — they were never a classification problem; the catalog
confirms they're seen correctly, so that fix is the separate multi-cell scan.)

## v0.8.17 — probes: why potions and arrows read as "not there"

Two confirmed detection misses to pin down. He carries 5 health potions but the
log shows `potH=0` and he won't drink; and bodies that visibly hold arrows are
called empty by the loot scan. Both are classification/read questions that depend
on the live inventory (this is a Requiem list, which reworks potions), so this
build adds two temporary probes rather than a blind fix:

- `[potprobe]` — when a wanted drink finds nothing, dumps every alchemy item he
  carries with its raw MGEF archetype, primary actor value, food/poison flags,
  and what our classifier decides. This reveals the exact archetype Requiem's
  restore potions use so `PotionRestores` can be widened to match — precisely,
  without also swallowing fortify/regen/poison.
- `[arrowprobe]` — when an arrow scan calls a lootable body empty, if that body
  actually holds ammo it logs it (name + isBolt + count). Tells us whether
  `HasLoot(arrows)` is wrong on a body that has them, or the arrow bodies are
  simply never in the scanned set.

Probes only; no behaviour change. Removed once both causes are pinned.

## v0.8.16 — supply-state conditions in the combat table (#10)

The combat gambit editor can now gate on what the follower is *carrying*, not
just health bars. Five conditions that previously existed only in the logistics
table are now offered in the combat table too: **Health / Stamina / Magicka
potions below**, and **Arrows below** / **Bolts below**. So you can finally
author things like "Arrows below 5 → Equip melee weapon" (the archer who draws
his sword when the quiver runs low) or "Health potions below 2 → Cast heal on
self" (conserve the last potions with magic). The engine already evaluated these
the same in either table — this exposes them in the editor. The drink-potion
actions were already available in the combat table; this completes the set.

Includes everything in v0.8.15 (full-body loot on arrival + idle reassess).

## v0.8.15 — strip the whole body, and reassess skipped ones when idle

The arrow probe pinned it: he'd walk to a corpse for its GOLD, take only the
gold, flag the corpse "done" (blocklisted ~25s), and walk back to you — leaving
its Iron Arrows behind in a body now out of arm's reach AND on the skip-list, so
the arrow gambit could never return for them (the 340u/382u arrow bodies in the
deck test). The trip was single-category.

Now on arrival he **strips the whole body**: everything his gambits currently
want — gold, arrows, potions, gear — in the one visit he's already standing
there for, respecting each gambit's condition (arrows only if he's short, etc.).
Only then is the corpse genuinely done.

And, per your note, a fairness backstop: when a follower finds nothing to do for
a few idle seconds, he **wipes the travel skip-list and reassesses every nearby
body fresh** (bounded to once per ~15 s so a truly unreachable body can't spin
him). A body that was skipped because he couldn't path to it from where he stood,
or one mistakenly shelved, gets another look once he's moved.

## v0.8.14 — arrow probe: dump what the scan actually sees on a body

v0.8.13 proved the arrow scan runs and finds every nearby corpse "empty" — even
one that visibly holds "Iron Arrow (9)" and that he already looted the gold from.
The `IsBolt()` classification is verified correct against the CommonLibSSE-NG
source, so the miss is in the runtime inventory read, not the logic. This build
adds a temporary `[arrowprobe]` line: during an arrow scan, for every corpse
within 300u it logs the body's id, distance, dead/container flags, item count,
and EVERY ammo item with its `isBolt` flag + count. That distinguishes the three
possibilities in one look — the body isn't being scanned (cell/range), the
inventory read comes back without the arrows, or the arrows carry an unexpected
flag. Probe only; no behaviour change. Removed once the cause is pinned.

## v0.8.13 — see which loot category he's actually hunting

Chasing "he has zero arrows and ignores the arrows on bodies, but loots potions
and gold just fine." Everything in the arrow path checks out on inspection, so
this build makes the loot diagnostic tell the truth: the `[loot]` line now names
the category it scanned (`cat=arrows`, `cat=gold`, …) and logs once **per
category** instead of once per follower — before, only the first category
scanned in each 10-second window ever reached the log, which is exactly why the
arrow scan's own counts were invisible. One deck cycle now says whether the
arrow scan finds nothing (detection / bodies genuinely carry no arrows) or finds
them but the walk/pickup fails.

Also: the attack log prints the chosen foe's HP% so we can confirm the "fight
the weakest foe" gambit is picking the lowest-HP target (it fires correctly —
the "already on that target" lines just mean no visible switch was needed).
Router polish: the no-progress "unreachable" giveup relaxes 4s→7s to stop
transient stalls being mistaken for dead ends, and the per-body WALK diagnostic
is rate-limited so it no longer floods the tick.

## v0.8.12 — the freeze fixed at the source: skip bodies that are off the navmesh

The research settled why he'd freeze staring at a body: it was physics-settled
OFF the navmesh (clipped into geometry, on furniture, a disconnected ledge), so
the game's pathfinder had no ground triangle to aim at and never even started
walking. No amount of package tuning fixes that — the destination itself is
unwalkable.

Now MFO checks the game's own navmesh data *before* he sets out: if there's no
navmesh near a body, it's skipped on the spot (a quick data lookup) instead of
committing him to a walk he can't complete. So he only heads for bodies he can
actually reach. Tunable on the Behaviour Layer page ("Off-navmesh skip"); the
diagnostic now also logs the engine's own path-speed, so if any freeze remains
we can see instantly whether it's a path that never built.

## v0.8.11 — audit hardening (crash-safety, the freeze softened, cleanups)

A multi-agent audit of the whole loot system produced these fixes:
- **Crash-safety:** on save-revert/load the background worker could write to state
  the main thread was clearing at the same moment (undefined behavior). The worker
  is now stopped and drained before any of that happens.
- **The "stares at a body across a wall" freeze is softened:** if he can't reach a
  target, he now gives it up quickly instead of committing to it, and only ever
  heads for a body that actually holds what he wants — so far fewer bad trips. (A
  proper "don't even pick unreachable bodies" pass is next — see below.)
- Fixed a regression where he'd abandon a corpse you were browsing via QuickLoot.
- He no longer drops a *near* body in favor of a far one when many are around.
- Config/MCM cleanups (slider range, a stray default, tooling coverage).

Next: the routing research found there's no native "walk here" call — every mod
uses the package approach MFO already does; the freeze is bodies physics-settled
OFF the navmesh, which no package can path to. The fix is to detect off-navmesh
bodies and skip them before he sets out (using the game's navmesh data), landing
in a follow-up.

## v0.8.10 — only walk to a body that actually has what he wants

A follower no longer walks over to a corpse or barrel that doesn't hold the thing
his gambit is looting for. Before, he'd trek to every nearby body, arrive, find no
arrows (or whatever he was after), and move on — wasting the trip and, when the
next candidate was unreachable, stalling. Now the check happens before he sets
out: bodies without the wanted category are skipped outright, so he only makes
trips that pay off. This also sharply cuts the unreachable-body problem, since a
far body with nothing he wants is never a target in the first place.

## v0.8.9 — walk-to-loot works; stop over-committing to unreachable bodies

v0.8.8 landed the real fix — the follower now genuinely walks to and loots
reachable bodies. This tunes what was left: he'd keep trying to reach bodies with
no path to them (across a gap, another level) for ~15s each and cycle to the
next, so a loot run dragged to its 60s cap and he wouldn't come back to you. Now
if he isn't actually moving toward a body (no path), he gives up in a few seconds
and moves on — so the run ends and he returns to you promptly. (Distances he'll
walk are still the full confidence leash; this only drops ones he physically
can't reach.)

## v0.8.8 — walk-to-loot: the actual root cause fixed

The diagnostic from v0.8.6/0.8.7 finally caught it: the follower was never
receiving MFO's travel package at all — he stayed on his normal follow package
even though MFO's priority read 60. The reason: the engine decides which quest
"owns" a follower at the moment his alias is filled, and MFO was raising its
priority to 60 *after* that, which the engine ignores for ownership. So he never
walked to loot; he only ever grabbed what he ended a fight next to.

Fix: MFO now claims the follower at the correct priority from the start (the same
way the combat-casting system — which works — always has), and releases him by
handing the slot back rather than lowering a number that doesn't un-claim him.
This is the fix the whole walk-to-loot saga was circling; the built-in diagnostic
will confirm it on the next run (the follower should finally close the distance to
bodies).

## v0.8.7 — combat no longer interrupted by loot runs (+ carries the v0.8.6 diagnostic)

Fixed a real regression from batching: a loot run could last up to 60s and ran
**right through a fight** — the follower stayed committed to looting at high
priority instead of fighting, which read as "less aggressive in combat." Now the
instant a follower is in combat, any loot run he's on ends immediately and he
fights. (Includes the v0.8.6 walk diagnostic and the leash-based walk range, so a
single relaunch gets everything.)

## v0.8.6 — walk-to-loot diagnostic + wider walk range

Walk-to-loot still isn't working — the follower doesn't head for bodies a short
walk away, only grabs what he ends a fight next to. Everything checkable is
correct (the travel package matches a vanilla one that works, the wiring, the
priority), so this build adds a targeted log line to catch the one thing left:
whether he's actually running MFO's travel package or still just following. That
tells us exactly what to fix next instead of guessing.

Also: walk range is now the **confidence leash** (was a fixed 768u / one room),
so bodies anywhere in his leash are fair game — the old cap was hiding most of
them.

## v0.8.5 — batched loot runs (no more return-trip per corpse)

A follower now loots in a **batch**: he stays out and visits each valid corpse in
one trip — closest first — instead of turning back toward you after every single
pickup. He returns when the loot is exhausted, when you walk away (past his
confidence leash), after a safety time cap, or when combat starts.

- **Waits for your dibs mid-run.** If nearby loot is still under your first-dibs
  timer, he holds in place briefly (re-checking) rather than abandoning it — so a
  corpse you were about to pass on still gets picked up once you clearly move on.
  Tunable: *Batch: wait for your dibs* (Behaviour Layer, default 4s; 0 = never
  wait).
- **Can't run away.** *Batch: max loot-run length* (default 60s) caps any single
  run, and the confidence leash still bounds how far he'll roam from you.
- **Respects your gambits.** A "Wait" rule that starts matching mid-run stops the
  batch immediately, and opening a container or crouching pauses it (he doesn't
  loot out from under your menu or blow your stealth) without ending the run.
- No carry-weight cap — MFO only ever takes the item types your gambits name, so
  there's nothing to hoover; carry weight stays your load order's business.

## v0.8.4 — walk-to-loot release fixed for real: hand the follower back by priority

v0.8.3's `ResetQuest` clear was disproven on the deck exactly like v0.8.2's
`ForceRefTo(None)` before it — the readback fired again (`CLEAR STILL FAILED`),
the follower stayed latched and wouldn't follow or come on cell change. Two
failed clears settle it: **a force-filled alias ref is sticky and can't be
cleared** from native code with the natives available. A first attempt to work
around that with a package condition was caught in review before shipping — it
would have *rooted* the follower (the engine keeps him claimed with nothing to
do), so it was scrapped.

### The real fix — release by quest priority
The follower is handed back by making MFO's loot quest **outrank** the follower
framework while he walks to loot (so MFO drives him) and **drop below** it to
release (so the framework takes him back and he follows). The alias staying
filled no longer matters — whoever outranks wins. This has no "stuck" state: he's
always driven by *someone*, and if anything about the hand-off ever misbehaves it
fails toward "doesn't bother looting," never toward "frozen." A save stranded by
an older build heals itself on load.

### Edge cases closed
- Turning the loot/logistics toggle **off mid-walk** now releases him immediately
  instead of stranding him.
- A follower **dismissed mid-walk** is properly let go (otherwise he'd keep
  wandering off to loot, even across reloads).

### Also fixed — the corpse-shuffle churn
A follower who arrived at a corpse and found nothing he wanted used to be re-sent
to the same nearby corpses every few seconds, forever. Arriving at a source
(looted or not) now puts it on a short cooldown, so he moves on.

## v0.8.3 — walk-to-loot actually works: reliable clear + real Run gait

The v0.8.2 walk-to-loot fixes were disproven by the deck in one run (the
readback discipline earned its keep). Two blind theories were wrong; both are
now settled against evidence.

### The follower stopped going unresponsive — the alias clear is fixed for real
v0.8.2 cleared the loot alias with `ForceRefTo(None)`. The guard readback fired:
`NATIVE CLEAR DID NOT TAKE` — that native (id 25052) KEEPS the current ref when
passed null, so the alias stayed latched and the follower stayed stuck. v0.8.3
clears via **`ResetQuest`** (id 25014), which empties every alias fill natively;
the bare start-game-enabled loot quest re-inits empty and running for the next
fill. A second readback confirms the quest is still running after the reset.

### The follower runs to loot now instead of a slow walk
The travel package's `preferredSpeed = Run` byte was **inert** because the
"Preferred Speed" general flag (`0x2000`) wasn't set — it had been mislabelled
"AlwaysSneak" and cleared. Proven by scanning all 5,961 Skyrim.esm packages
(speed only varies when `0x2000` is set). v0.8.3 sets the flag, so followers
actually **Run** to loot and rejoin. `0x2000` is not the ignore-combat bit, so
combat still interrupts looting.

### Recovery
A follower latched by a v0.8.1/v0.8.2 build frees itself on the next load
(`ReleaseAll` now runs the working `ResetQuest` clear).

## v0.8.2 — loot priority redesign (Claim-and-Release) + walk-to-loot fixed

Two big loot changes: the "first dibs" system is redesigned into **Claim-and-
Release**, and the v0.8.1 walk-to-loot strand is fixed.

### Claim-and-Release — how a follower decides it may take your loot
The old flat first-dibs timer measured the wrong thing (it counted from when the
FOLLOWER saw a corpse, so a distant player lost the race). Now every source is
under an implicit **player claim**, released only by evidence about YOU:
- **Tiers**: consumables (arrows/potions) are free; ordinary gear waits a short
  grace; **valuables (gold) release only by rejection, fair-chance, or
  abandonment** — no timer a distant player can lose to.
- **Rejection**: you looted the source and moved to another / walked away / went
  quiet after your last take.
- **Fair chance**: you were near AND facing a source (3× while you're viewing it)
  and left it; or you never came near it at all (abandonment cleanup).
- **Convergence yield**: a follower never walks to loot right next to you — you
  win the race for the corpse you're heading to.
- **QuickLoot-aware**: detects QuickLoot in the load order; your crosshair on a
  corpse (its HUD up) counts as considering it — the follower won't take from a
  source you're viewing, and yields to it. Falls back to the vanilla menu.
- Tuned on a new, grouped **Loot Priority** MCM page.

### Walk-to-loot fixed (the v0.8.1 strand)
A follower went **unresponsive** — a priority-60 loop toward loot it could never
reach and never release (root cause in ENGINE_NOTES §0.34):
- **Native alias release** (`ForceRefTo(None)`) — the old Papyrus-VM `Clear`
  failed every time (MFO's aliases have no script) and left the follower latched.
  Also **self-heals a latched v0.8.1 save on load**.
- **Walkable radius** (`fTravelRadius`, ~one room) with a distance-scaled
  deadline; farther loot is left. **Closest loot first**; a **travel-failed skip**
  stops the churn.
- Followers now **run** (not walk) to loot. Off-switch: **Walk to loot** in the
  Behaviour Layer MCM.

## v0.8.1 — board font parity, elemental weakness gambits

- **The board bakes real typefaces** (MEO's, at backbuffer scale) instead of
  scaling ImGui's default bitmap font — the header uses the display face, the
  body the text face. This was the last "less polished than MEO" tell. Falls
  back to the scaled default if the fonts are missing.
- **`Foe: weak to fire / frost / shock`** — foe-selector conditions that pick
  the nearest enemy whose resistance to that element is negative (a race trait
  or active weakness), read from the actor's own resist value. Pair with a
  matching Cast action for the FFXII "exploit the weakness" play.
- Docs: README brought current to the shipped feature set (it was stuck at
  v0.3.0), and its stale figures corrected (real slot counts per rank, the
  Field Orders board name, first-dibs timing, removed the cut spell-teaching).

## v0.8.0 — logistics comes alive: followers loot, walk to it, and know their limits

The arc from v0.6.0 (the cast pipeline) through the v0.7.x field builds and into
Option A. The headline: the logistics table is no longer inert — followers
restock, loot, and equip on their own, they *walk* to loot instead of teleport-
grabbing it, and how far they'll range is governed by a new core tenet.

### The confidence leash — a core tenet (invisible strings)
How far a follower operates *from the player* is never a fixed number; it is a
live readout of how confident they are to survive alone (`native/Confidence.h`:
`Of()` → `LeashRadius()`, from vitality, dampened in a fight). Bold in an easy
zone (leash grows, they push ahead and range out to loot), cautious in a hard
one (leash shrinks, they fall back to the player). The player never sees a
number — they feel it. One primitive; the loot leash today, combat target-
distance next. See DESIGN "Core principles".

### Option A — engine-pathed loot acquisition
Followers now WALK to loot via the vanilla Travel package (`MFO_LootQuest` +
`MFO_TravelPackage`, authored from Skyrim.esm's own shape — ENGINE_NOTES §0.33),
transfer on arrival, and release cleanly (arrival / combat / timeout / vanished
target / open container menu / sneak / a global stale-expiry / unconditional
release on every load — the #55 alias latch self-heals). On by default; one MCM
toggle off. Off-AE falls back to arm's-reach transfer.

### Looting, made real
- **Dumb consumables**: loot arrows, bolts (own gambit), and potions (any, plus
  per-resource loot health/stamina/magicka). Arrows/potions skip first-dibs.
- **Loot gold**, and **skill-based "loot better equipment"** — weapon upgrades
  judged in the follower's dominant weapon-skill class (1H/2H/Archery); armor by
  rating. Casters don't hoover up random weapons.
- **Locked containers are Lockpicking-skill-gated** (Novice→Expert; Master and
  key-required never open). Owned loot stays a crime.
- **Confidence leash + "don't loot while you're sneaking"** courtesy.
- Loot reach is a tunable `fLootRadius` (~4–5 rooms); leash bounds and walk-to-
  loot live on a new **Behaviour Layer** MCM page.

### The board — "Follower Overhaul / Field Orders"
Retitled, trimmed to the two player-facing tabs (Followers, Gambits), restyled
on MEO, and given a full-width read-only gambit summary so a whole rule is
legible on the Steam Deck. Per-rule value steppers for pad control.

### Fixes
- **MCM finally works**: the missing compiled `MFO_MCM.pex` (the config's
  binding script) now ships, the settings store seeds every control (no more
  −1), and controls bind by `id` (ENGINE_NOTES §0.31).
- Removed the `MFO_ProbeSelect` diagnostic switchboard (no longer needed).

## v0.6.0 — followers cast, with animations, at targets you choose

**The headline: MFO does not cast. It arranges the conditions and the
follower's own AI casts** — animated, magicka-arbitrated, correctly aimed,
because it is the vanilla path. Confirmed in the field: with a spell MFO put in
his hand, Cosnach cast it himself, repeatedly, with the normal casting
animation and real magicka cost.

Three cast *verbs* were refuted getting here — `CastSpellImmediate` (all four
casting sources), `Projectile::LaunchSpell` (no projectile on a self-heal), and
`DoCombatSpellApply` (the Papyrus twin of the first). The verb was never the
missing piece. `ActorMagicCaster` is driven by the animation graph, and the
graph is driven by the follower's combat AI, so the real question was what
*state* an NPC needs before its AI casts: a spell in hand, and a target.

### New — the attack verb

`act.attack`, with foe selectors that also choose the target, the way FFXII
gambits do:

```
Foe: lowest HP   ->  Attack
Self HP < 60%    ->  Cast <spell>
Always           ->  Wait
```

Candidates come from the follower's own combat group, not a world sweep — the
engine already knows who is in the fight, and a swept list could name someone
they are not engaged with.

**Papyrus cannot express commanded targeting at all.** There is no
combat-target setter in `Actor.psc`, SKSE, po3 or PapyrusUtil — only
`StartCombat`/`StopCombat` and a *getter*. A survey of ten installed modlists
found zero Papyrus attack commands. So MFO hooks `Character::UpdateCombat` and
writes the latched target after the engine's own re-pick, the technique the
open-source SmartTargetingNPC uses. This also settles a question two field
sessions failed to answer: **targets are not sticky** — the engine re-picks
continuously, which is why the hook re-asserts rather than writing once.

Off by default (`bCommandTarget`), because it installs a vfunc hook. VR is
refused outright: the vtable index is verified for SE/AE only.

### New — the equip policy

A gambit spell goes in the follower's **off hand**, which costs a one-handed
fighter nothing. A displaced shield returns when they take a hit, when combat
ends, or on dismissal — deferred, never abandoned. A two-hander must be stowed,
so that swap is debounced.

### New — cast rate limiting

`fCastCooldown` (4s). MFO cannot tell a combat AI to cast less often, but it
decides what is in the follower's hand and they can only cast what they hold.
A cast takes the spell back; the cooldown decides when it returns. Applies to
the follower's own casts as well as MFO's.

`fMagickaReserve` exists but defaults to **0**, deliberately: a follower
healing to stay alive is exactly who a floor gets killed.

### Also

- The evaluator's cadence is its own constant, not the diagnostics pump's — a
  HUD refresh rate was silently setting how fast gambits fire.
- Suppression is positional: a higher-priority rule always preempts.
- The combat table runs only in combat.
- HP% is computed over true maximum, including fortify effects, so heals no
  longer fire late on buffed followers.
- Rapport no longer loses a follower's kill when the fight ends before the
  death event is processed.
- The co-save round-trip is proven in the field: a save carrying rapport
  reloaded intact.

## v0.5.1 — first field session. M1 closed, one hypothesis dead.

### M1 is closed

A save carrying Cosnach at **rapport 5 reloaded with the record intact** —
nothing dropped, no collisions, rapport preserved. The co-save is the
"mod ate my save" subsystem; it had been reviewed four times and never once
executed end to end. It is now proven (ENGINE_NOTES §0.11).

### The animation hypothesis is refuted

**All four casting sources — kLeftHand, kRightHand, kOther, kInstant — cast
with no animation.** `CastSpellImmediate` is a silent effect application
regardless of which `MagicCaster` issues it; the casting source is not the
variable. `iCastSource` stays configurable but defaults back to `3`/kInstant,
which at least names what happens.

A visible cast still needs `LaunchSpell`, Papyrus VM dispatch, or a separate
animation event. **This is now the biggest open problem in the mod** — the core
loop's most visible action has no visible action (§0.10).

### The engine does not gate casts on magicka

Casting works **unlimited times at zero magicka**. So `CastSpellImmediate`
deducts while a pool exists, then casts free forever — which makes MFO's own
`CalculateMagickaCost` pre-check **the only gate that exists**, not
belt-and-braces. Delete it and every follower is an infinite spell battery
(§0.9).

### Rapport was losing followers' kills

Cosnach fought and killed a fox ~2792u away and earned nothing.

The shared-kill test ran from a **queued** death task and asked `IsInCombat()`
— but killing the last enemy *ends combat*, so a follower who fought the whole
battle reads `false` at the only instant the test gets to look. The radius
fallback then measured to the **player**, and the follower who did the work is
the one most likely to be far from them. Both checks failed.

Combat participation is now **sampled on the sweep** and tested as "was
fighting within `fSharedCombatGrace` (15s)", with proximity to the **victim**
as a third clause. The zero-credit case now logs why, per follower (#51).

Review caught that the first fix left the same signature reachable: a
held-but-unresolvable handle was skipped by both the award loop and the new
diagnostic, though neither needs the actor — the kill test is a FormID compare
and `Award` takes a FormID. Both are index-aligned now, and the unresolvable
case reports itself instead of printing an empty block under "0 credited".

### The HUD contradicted itself

It read `0 rap` directly above a follower showing `5`. Both correct — session
versus lifetime — but together they say the mod lost your save. Now labelled
`session` and `total` (#52).

### Also

`StartCombat` **did not take**: returned OK, then `NEVER ENTERED COMBAT`. The
earlier result that looked like success was the confounded single-target case,
so §4.7 standing orders rest on nothing measured (§0.12). Seeds are enabled in
the shipped INI.

## v0.5.0 — the evaluator. Gambits execute.

**First playable.** A follower with a rule list now acts on it: round-robin one
follower per tick, top-down first-match, one action per tick — the FFXII
contract. Conditions are self HP/MP/SP % and player HP % plus `always`; actions
are cast (self/target) and wait.

Set `bSeedEvaluatorRules = 1` to seed `Self HP < 40% -> Cast Healing` and
`Always -> Wait` onto every follower; the rule board arrives at M7.

**The evaluator only runs in combat.** A wounded follower standing in town is
supposed to do nothing — the combat and logistics tables never interleave
(§4.8). Testing out of combat reads as "broken" when it is behaving.

### Deliberately not in this slice

Jitter, urgency tiers, distance LOD, standing orders, drink/equip, and the
logistics table. Each is designed and each is its own build. Standing orders in
particular wait on §4.7's retention question, which is still confounded — no
target latch gets built on an unproven assumption.

### What the reviews caught

Two Fable passes. The first found two blockers that would have made the whole
milestone test nothing:

- **The seed was never called.** Defined, wired to a config key, documented in
  the test guide — and no call site. With no board until M7 that seam is the
  only way a follower ever gets rules, so every follower would have returned on
  an empty table forever. M5 would have shipped, run, logged nothing, and
  looked like a design failure.
- **The "133 ms" tick ran at 500 ms.** It rode the diagnostics pump, whose
  interval existed to pace a HUD redraw, so the self-gate was dead code — the
  caller never arrived faster than it. When that constant moved 2000 -> 500 ms
  for the Field Kit, it silently quadrupled how fast gambits fire. A display
  constant must never set behaviour (#46).

Also fixed: suppression was **absolute**, so a just-fired low-priority rule
deafened a follower to a higher one for the full window — the priority
inversion #26 exists to forbid. It is positional now; a higher rule always
preempts. HP% used `GetPermanentActorValue`, which omits the temporary modifier
where fortify-health gear lives, so heals came late on any buffed follower
(#47) — the Field Kit's bars now share the one formula, because a HUD that
disagreed with the evaluator would lie exactly when consulted. Repeating
failures logged every tick with a synchronous flush (#48). A fast revert->load
could leave two pump threads running (#49).

The verification pass then caught a bug the **first round's own fix**
introduced: the new identity-keyed cursor did not advance past a held null
handle, so one unresolvable follower pinned the entire rotation for up to
~1.6 s — in combat, the only time it matters (#50).

### Engine findings

- **`CastSpellImmediate` DOES deduct magicka** (§0.9). This refutes the
  standing assumption that it is a free scripted cast. §5.3's competence gate
  is load-bearing rather than decorative, and MFO must **not** hand-write a
  deduction — the engine produces that state (#16).
- **`kInstant` is by name the no-animation caster** (§0.10), which likely
  explains the missing cast animation. The probe now fires one variant per
  casting source, and `iCastSource` selects the winner **without a rebuild**.
  This may make `LaunchSpell`/VM dispatch unnecessary.

Folded in from the held batch: level-relative boss detection, `PickFoe`
candidate counting.

## v0.4.1 — review fixes. **Save on this one, not v0.4.0.**

v0.4.0 shipped without a Fable review — a process failure, since the same
build changed the save schema. The review that should have run first came back
with the schema **verified clean** (read/write symmetry, the v1 compatibility
reader, version handling and hostile-input bounds, checked against the actual
shipped v0.1.0–v0.3.0 writers) but found four real defects around it. Two of
them cost you data during ordinary play.

**A follower's miss-streak survived across saves.** The hysteresis that stops
a transiently-unresolvable follower being dropped is keyed by FormID — and the
same NPC has the same FormID in every save, so a streak from one save applied
to the next. From the second save load onward, that re-opened the exact
"a kill in the drop window credits nobody" bug the hysteresis exists to fix.

**A cloned or spawned teammate's first kill created a doomed record.** The
award path used the unguarded record accessor, so an actor with a runtime
FormID — routine in a big load order, and *not* covered by the summon check —
got a record that the save layer then had to throw away every single save.

**The downgrade warning is now on screen**, not only in the log. If a save was
written by a newer MFO than the DLL reading it, saving over it destroys that
data — and nobody reads a log until after they have lost something.

Also: a write failure now says plainly that the save's MFO data is truncated
and to re-save, rather than looking normal; a follower whose handle briefly
fails to resolve now gets the hysteresis hold instead of vanishing silently;
truncation at the rule and override caps logs instead of dropping quietly; and
`iBossLevelDelta` is floored at 1, since 0 would have made every equal-level
kill a boss.

Everything in v0.4.0 below still applies.

## v0.4.0 — scope cut, co-save v2, and the probe harness

**Read this one before installing** — it changes the save schema and removes a
feature.

### Spell acquisition is out of scope

MFO does gambits. It no longer teaches followers spells. *A Fun Way To Level
Followers* (TrumanAE, SKSE, open source) already does that properly — skill
points per player level, perks and spells at 20/40/50/60/80/100, configurable
so it works with any perk or spell overhaul.

The split: **they own acquisition** (how a follower comes to know Fireball),
**MFO owns deployment** (when they should cast it).

This is synergy rather than deconfliction. MFO's aptitude gate reads a
follower's skills to decide what vocabulary they're offered, and without a
levelling system those values barely move — the gate is nearly static. Their
mod makes it live: spend skill points, and MFO's vocabulary opens in response.

Deleted with it: the tutored-spell ledger, the revoke path, the
`RemoveAddedSpells` backstop, and the reconcile-on-load. **That was the
largest remaining surface in MFO that could damage another mod's state** — a
mis-scoped revoke would have eaten spells another mod granted. MFO now adds no
spells or perks to any actor, ever.

MFO's derived action vocabulary still reads whatever a follower knows, so a
spell from a levelling mod, a perk overhaul, or a quest all appear identically
with no patch.

### Co-save schema v2

**v1 saves are still readable**; the v1 reader consumes and discards the old
tutored block. Nothing is lost that MFO owned, and spells already on an actor
are untouched — MFO never held them, it only remembered granting them.

*Why the bump:* the tutored block was first removed at v1 without one, on the
reasoning that no save had ever held an MFO record. That was true when written
and expired as soon as saving with the mod active was on the table — and
v0.3.0 already writes v1 *with* that block, so a v0.3.0 save read by this
build would have misparsed with no guard able to catch it. "Nobody has data
yet" is a fact with an expiry date; a version number is not.

What a save actually carries right now is 14 bytes per follower, all
fixed-width: FormID, rapport, rank, table count, two zero rule counts, zero
overrides. No strings, no form references — those arrive with the evaluator.

### The Probe tab (M4)

A new tab in the Field Kit: pick a follower, fire one engine primitive, watch
what happens. Nothing persists. Its centrepiece is the **target-retention
watch** — press *StartCombat*, and it samples the follower's actual combat
target every tick, comparing by handle rather than name, distinguishing "the
commanded target died" (invalidation) from "the engine re-picked" (a problem),
and reporting how long the commanded target actually survived.

That single measurement decides whether the standing-order model in the design
holds or needs a refresh cadence, and it cannot be answered by reading any
source.

**It already produced its most valuable finding before running.** Review
established that **five of the twelve primitives the design lists as Tier B
have no C++ binding in CommonLibSSE-NG**: `KeepOffsetFromActor`,
`ClearKeepOffsetFromActor`, `SetDontMove` and `DoCombatSpellApply` are
Papyrus-only, and `StartCombat` exists only in po3's fork. The research was
sound — the Papyrus surface named the right engine flows — but *"a Papyrus
native exists"* and *"I can call it from C++"* are different claims and only
the first had been checked. Positioning probes are therefore **blocked** and
shown as such in the UI rather than quietly omitted.

### Also
- `bSeedTestData` writes synthetic rules onto a player-keyed record. Leave it
  **off** for real play; turn it on to exercise the co-save.
- FormID `0x802` shipped as the granted-spell keyword and is now reserved and
  unused. FormIDs are never recycled.

## v0.3.0 — kills actually get credited

Field fixes from reading a real session log. The reported symptom was "boss
multiplier didn't apply"; the log showed something worse underneath it.

**Kills were being silently dropped.** A follower transiently absent from the
engine's high-actor list was removed from the tracked set instantly — and the
death sink refreshes that set *before* awarding, so a kill landing in that
window credited nobody at all. The log signature was a remove and re-add
**117 ms apart**, far tighter than the 500 ms refresh. Now held for three
consecutive missed sweeps: one miss is not evidence of absence.

**The log had gone blind to Rapport.** Awards only logged on a rank change,
so the overlay made Rapport visible in game and invisible in the log in the
same release. Every award and every credited kill now logs at info —
including the zero case — with victim name, both levels, and classification.

**Boss detection was wrong, as reported.** `IsUnique()` means *named one-off
actor*, not *hard fight*; generic dungeon bosses are leveled and not unique,
so a bandit chief with a boss bar read as standard. Now unique **or** at
least `iBossLevelDelta` levels above you (default 5) — relative, so a chief
is a boss at level 8 and an inconvenience at 50. Tunable in `MFO.ini`.

**The panel explains itself.** Measurements now shows the last credited kill:
name, its level, your level, the classification, and what was awarded to how
many followers. "Why wasn't that a boss?" is answerable without a log.

*Two of these became this project's first invariants earned in the field
rather than inherited from a sibling (#22i, #22j).*

*Note: `[cosave] saved 0 follower record(s)` appeared in a session with no
manual save — autosave and quicksave fire regardless of intent. Harmless with
seeding off, but worth knowing that "not saving" is not under your control.*

## v0.2.0 — the Field Kit (in-game overlay)

Everything v0.1.0 had, now **visible in game**. Reading a log after the fact
is a hopeless loop for a behaviour mod, so the overlay was pulled forward
from M7.

**⚠ First code hooks in the project.** Three trampoline hooks (D3DInit,
DXGIPresent, InputDispatch) install at plugin load, **before** the renderer
exists, and they install regardless of any INI setting. Offsets and thunk
shapes are transcribed from MEO's shipped, field-validated implementation and
were confirmed against it in review — but **this build has never run**. If the
game hard-crashes on launch, this is the suspect, and `bShowHud = 0` will not
help because the hooks are already in by then.

**The HUD** — top-right, passive, draws every frame and takes **no input**, so
it stays readable while fighting. Per follower: name, combat flag, rank,
rapport, live health/magicka/stamina bars, distance. Plus session kills,
rapport, and rapport/hour. This is the primary observation surface; the panel
is for detail. `bShowHud` in `MFO.ini`.

**The panel** — the **Field Orders** power opens it; Esc, gamepad B, or the
shout key closes it. Three tabs:
- *Followers* — the full table, **including retained-but-inactive records**,
  so dismissal being non-destructive is visible rather than taken on faith.
- *Measurements* — the two numbers this build exists to take: the
  teammate-filtered combat-event rate, and kills/rapport per hour. The
  rapport/hr figure turns amber below 30, which is the case where
  `BALANCE.md`'s rank ladder needs redoing.
- *Config* — what is actually in force, plus quirk-table resolution.

Full controller parity throughout (family standing rule): gamepad nav, stick
edge-triggered into d-pad, B to close.

**Also**
- Test seeding moved to `bSeedTestData` in `MFO.ini`, **default off**. With it
  off and no saving, **MFO writes nothing anywhere** — it reads state and
  draws it.
- Detection refresh 2 s → 500 ms so the HUD reads as live.
- Log now lands in the MO2 profile's `overwrite/SKSE/Plugins/MFO.log`,
  alongside every other plugin's, rather than in the wine prefix.

**Validated in-game (from v0.1.0 testing)** — follower detect/undetect across
three cycles with records retained, form resolution to the ESL band, SPIT
type 3 correct for a castable lesser power, `TESSpellCastEvent` firing for
lesser powers. See `ENGINE_NOTES.md` §0.

**Still unproven, deliberately:** the co-save (no saving with MFO active yet),
all of Rapport (no kills have occurred), and combat-event volume.

**Review:** Fable pre-CI review found one blocker — a software mouse cursor
that would have been composited over ordinary gameplay from the moment the
renderer came up — plus four majors including a mutex-nesting violation of the
project's own invariant and an input-swallow divergence from the reference.
All fixed before this build.

## v0.1.0 — first testable build

Detection, Rapport, and the co-save. **No gambit execution yet** — the
evaluator arrives at M5. This build exists to prove the foundations before
anything is built on them.

**What it does**
- Loads, resolves its forms, and grants the **Field Orders** power.
- Detects followers framework-agnostically: teammate status as the gate, plus
  a quirk table that revokes eligibility for custom followers who signal
  dismissal their own way (Inigo's `WaitingForPlayer == -1`, Vilja's and
  Tindra's factions). Summons excluded by default and never persisted.
- Accrues **Rapport** per follower from shared kills — combat state carries
  the archery case, a radius fallback carries the stealth case — with boss
  ×5 and dragon ×10, and ranks I–V driving both slot ladders.
- Round-trips all of it through the SKSE co-save, with `ResolveFormID` on
  every FormID and per-rule disable (never whole-list drop) for missing
  targets.

**How to observe it.** There is no UI until M7. Press **Field Orders** at any
time to dump a full state report to the log: config in force, the quirk
table, active followers with their eligibility flags, every stored record
including dismissed ones, and the two measurements this build exists to take
— teammate-filtered combat-event rate, and kills/hour.

**Log location.** `<MO2 instance>/overwrite/SKSE/Plugins/MFO.log`. MFO writes
game-root-relative so USVFS lands its log beside every other plugin's, rather
than in the wine prefix where `SKSE::log::log_directory()` would put it.

**Known and deliberate**
- Ships with seeded test gambits on a player-keyed record (`kP0SeedTestData`),
  so the co-save round-trip is provable without a UI. **Use a throwaway save.**
- MCM settings need a restart; there is no live re-read sink until M7.
- The combat-exit survival award is not implemented; `fRapportSurvival` is
  parsed but unused.
- `BALANCE.md`'s rank thresholds rest on an **unmeasured** ~45 rapport/hour
  estimate. Taking that measurement is the main purpose of this build.

**Engineering notes**
- Zero code hooks. Event sinks only (death, combat, spell-cast), so there is
  no Address Library exposure in the core loop.
- Passed a pre-CI Fable review that caught two save-corruption paths — a
  diagnostic that inserted records via `operator[]`, and an unguarded path
  that could persist runtime `0xFF` FormIDs — plus six behaviour defects,
  including a kill counter that counted every death in the world rather than
  party kills.
