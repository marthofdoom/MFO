# MFO — Current Status & Handoff

> **LIVING DOC — KEEP THIS CURRENT.** This is the first thing to read to continue
> MFO from a clean slate. Update it *in the same change* whenever you: ship a
> release, open/resolve an issue, get a field-test result back from marth, or
> change the workflow. A stale status doc is worse than none — if you touch the
> project and don't touch this, you've left the next session a trap.
>
> **Last updated:** 2026-08-06 · **Latest shipped:** v1.0.34

---

## Continue in one screen

- **Latest shipped & deployed:** **v1.0.34 — "the mage update"** — deck-verified
  (DLL 6f8f64ef…), GitHub Release = Latest (CI run 31130000975; auto-trigger kept
  flaking so runs were `gh workflow run`-dispatched, and GitHub concurrency
  cancelled several — the release poll now re-dispatches on a cancel). SEVEN
  pieces in one cut:
  1. **Cast-control slider** (`iCastControl`, MCM, default 2 "ignore heals"):
     off → ignore buffs+heals → ignore heals → ignore self-heals → exact. The
     consent hook classifies the AI's own spell by the deliberating caster's
     vtable (Offense/Buff/Heal, heal split self/other by Delivery) and denies
     only the non-exempt categories. `CasterConsent::CastExempt`.
  2. **`MFO_CastStyle` cast stance** (reuses CSTY 0x832): a cast-latched follower
     is swapped to pure-mage; magicka-dry → pure MELEE, back on regen. Driven by
     the scheduler on the CombatStyle/UpdateCombat rails (v1.0.33's).
  3. **Cast in logistics** — `act.cast_*` runs in the logistics table too
     (out-of-combat self-buffs/candlelight/heals) via `Actuation::Fire`; a still-
     active self-buff is skipped (HasMagicEffect).
  4. **Teach spells from spellbooks** — the picker lists player-carried teachable
     spells "Name (spellbook)"; a 2nd click (DESTROYS-book warning) AddSpells +
     consumes the book (`EditKind::TeachSpell`, main-task ApplyEdits).
  5. **#56** combat-HUD X/Y margin sliders (clamped on-screen).
  6. **#61** fashionrim `bDollsMode` (armor acquisition+fitting off; sell stays).
  7. **#55 MCM fix** — ships `MCM/Config/MFO/settings.ini` (the defaults file MCM
     Helper registers from) + `audit_mcm.py` 5-touchpoint release gate.
  Fable-reviewed pre-release (caught a compile blocker + 4 picker MAJORs, all
  fixed). A4 (act.attack casts the set magic attack) SKIPPED per marth. **Field
  test pending** — watch items below.
- Field-test watch (v1.0.34): the cast slider at each stop (does a mage keep
  healing at "ignore heals" but cast the gambit for offense?); `[wstyle] … cast`
  + the dry→melee swap; a cast gambit in the LOGISTICS table firing out of
  combat; teaching a spell from a book (book consumed, spell set); fashionrim
  stops armor loot/robes but weapons still work; HUD sliders move the overlay.
- **v1.0.33** ("weapon-stance ownership") — Auri melee fix, deck-verified + FIELD-
  VALIDATED (combatStyle swap holds, 0 re-derives). **v1.0.32** ("mage fixes") —
  casting "drastically improved" per marth.
- **Compile is CI-only.** No local MSVC. The DLL only ever comes from a GREEN
  GitHub Actions `native` run whose `native/` tree matches HEAD. Never trust
  "it built" without `gh run list --workflow=native --status=success` + a
  headSha/tree match. (INVARIANTS #44; verify-ci-green memory.)
- **ESP is generated** by `python3 MFO_GenerateESP.py out` (Linux, no Creation
  Kit). ESP changes = edit the generator + regen. (ARCHITECTURE §8.)
- **Release** = two-phase `release.sh` (see its header). Phase 1 stamps+pushes→CI;
  phase 2 (after green) regens ESP, runs `audit_esp.py`, pulls the green DLL,
  packages `releases/vX/`.
- **Deploy** = unzip the package over `/mnt/gaming/modlists/custom-modlist/mods/MFO/`
  only (keep `meta.ini`), force a syncthing rescan, then poll the deck until its
  DLL sha256 matches the packaged one. (deploy-workflow memory has the exact
  commands + the syncthing key handling.)
- **Publish hygiene:** subagents do code→CI→deploy→tag; the **main agent** (in
  direct conversation with marth) runs `gh release create`. Keeps the public
  publish anchored to marth's authorization. (release-scope memory.)
- **Repo hygiene:** only `main` exists locally and on origin (feature/worktree
  branches were cleaned 2026-08-05). GitHub Releases exist for every tag
  v0.7.0→v1.0.31 (the v0.8.24–v0.8.48 gap and all v1.0.x were backfilled).

## Shipped this cycle (all deployed to deck + GitHub Release)

| Ver | What | Field-test status |
|---|---|---|
| v1.0.25/26 | Furniture-ejection fix: evict follower to an XMarker (never the player) + load-sweep un-latches old saves | marth reported furniture works; treat resolved |
| v1.0.27 | Hybrid forced-cast (force the gambit spell on AI miss) + line-of-sight gating (worker posts, raycast on main thread, cached verdict) | **pending** — watch `[los]`; does `HasLineOfSight` discriminate NPC→NPC on this runtime? (fail-open by design) |
| v1.0.28 | Exclusive cast control — while a cast gambit owns him, DENY every spell but the gambit's (was casting his own too) | **pending** — watch `[consent] … DENIED own spell` |
| v1.0.29 | Magic-user loadout — school robes + backup dagger + 2 MCM toggles (bMagicLoadout, bMageDaggersOnly) | superseded in part by v1.0.31; MCM toggles BROKEN (see #55) |
| v1.0.30 | Cast latch persistence — suppression holds through the cast cooldown, not just the fire (closes the between-casts leak) | **pending** — watch `[consent] … holding exclusive control through the cast cooldown`; no `(their own spell, not ours)` while a cast rule wins |
| v1.0.31 | Pure casters: mages loot NO rated armor (heavy or light), robes/clothing only; broadened school-robe detection; per-candidate apparel diagnostics; no junk picks | **pending** — watch `[loot] apparel …` diag lines; is Marcurio switching to his valid robe / out of heavy armor? |
| v1.0.33 | **Weapon-stance ownership** — equip gambits own the follower's live per-combat `combatStyle` (melee/ranged CSTY 0x833/0x834), applied on the combat thread from the UpdateCombat hook; reverts at battle end, hands off melee↔ranged. Stance follows the winning gambit, not the class (mage-melee / mage-ranged work). Default ON, `bWeaponStyleControl` INI kill-switch. | **pending** — watch `[wstyle] … OWNED/HANDOFF`; does Auri now hold the mace and stop plinking? Does she stop the bow↔mace flicker? |
| v1.0.32 | **Mage fixes** — forced casts actually fire (FindInput template-map fallback + static uids; CastAt only, cast_self stays barred: QNAM+t6 zero-precedent cell); cooldown-consulted permit (`permitAfter` on the latch — no more 4-casts-in-2.2s bursts); potion-exempt deny (only formType Spell is ever denied); flicker-proof latch (deny holds for the combat's duration; releases only on combat end/dismissal/revert); + INI-gated P1 combat-style probe (`bProbeCastStyle`, default OFF, new CSTY 0x832 `MFO_CastStyle`) | **pending** — watch `[cast] … FORCED … at …` (real animated force, no more `template input 'Spell'` errors), `[consent] … pacing` once per window, no suppressed combat drinking, `DENIED own spell` continuity across flickers; probe (only if armed): `[probe cstyle]` |

## Open field threads (awaiting marth's deck — pull the log, then diagnose)

The deck runs the game; MFO.log is at
`deck@marthdeck:~/Games/custom-modlist/overwrite/SKSE/Plugins/MFO.log`.
**CAVEAT: MFO.log is TRUNCATED every game launch** (one file, no backup) — the
session with a bug is gone after a relaunch. To catch something, marth must
reproduce it in the CURRENT session, then pull the log before the next launch.
(Offered but undecided: add an `MFO.log.prev` backup-on-load so sessions aren't
lost — small, safe; do it if marth confirms.) Deck may be ASLEEP → SSH timeout,
just retry ([[deck-sleeps-ssh-timeout]]).

1. **Auri melee switch — FIXED + FIELD-VALIDATED (v1.0.33, 2026-08-06).** Deck
   log: clean `OWNED -> ranged`, `HANDOFF -> melee`, `HANDOFF -> ranged` — she
   holds the stance, no more 6×/fight bow↔mace thrash. **`RE-DERIVED: 0`** — the
   live `combatStyle` swap HOLDS on 1.6.1170, engine never stomps it. That
   field-proves the CSTY-swap rails for Stage 2 (caster style swap) and makes the
   P1 probe fully unnecessary. History below:
   The deck log showed it was NOT the STATUS hypotheses: she carries a Glass Mace
   (dmg 84), `rule 1 act.equip_melee` fires, and she DID equip melee 6× — but
   the equips were *real* re-equips (a bow↔mace tug-of-war with her own ranged-
   weighted AI), so she flickered and never attacked. Fixed by weapon-stance
   ownership (v1.0.33): the melee gambit now swaps her live combat style so the
   AI stops re-drawing the bow. Confirm on deck: `[wstyle]` shows OWNED melee,
   she holds the mace, no flicker.
2. **P1 combat-style probe — largely OBVIATED by v1.0.33.** The probe's core
   MECHANICAL question ("does writing `combatStyle` on the live per-combat
   controller HOLD, or does the engine re-derive it?") is now answered for free
   by the v1.0.33 weapon-stance feature, which does exactly that swap on the
   RELIABLE UpdateCombat hook (all combatants) and logs re-derives explicitly
   (`[wstyle] … engine RE-DERIVED …`). So `bProbeCastStyle` need not be run
   separately for that. The probe's OTHER question (does a caster-forward style
   make him cast MORE / mobile) is cast-specific and folds into Stage 2 below.
3. **v1.0.31/1.0.32 field confirms** — see the shipped table's watch items
   (Marcurio switching to robes; forced casts firing animated, paced, potions
   flowing, no mid-flicker leak).

## Open issues (ranked)

0. **✅ SHIPPED in v1.0.34 (the mage update) — casting overhaul + full cast
   control.** Stage 2 landed as the graduated `iCastControl` SLIDER (not the
   single `bFullCastControl` toggle first sketched) + the `MFO_CastStyle` stance
   with magicka-dry→melee. Field test pending (watch items in "Continue"). Only
   the act.attack-casts-set-magic half was cut (marth). Historical detail below.
   HEADLINE — casting overhaul (next Nexus = "mage fixes"). Research (task #59)
   found the v1.0.27 forced cast had **never fired on deck** — it errored
   `template input 'Spell' is not declared on FE090820` and silently fell to the
   invisible `CastSpellImmediate`. **v1.0.32 SHIPPED (task #60), deck-verified:** fixed the
   dead forced cast (Packages.cpp `FindInput` — template nameMap on a MISS not
   just null; static uid fallback Spell=3/Target=4; scoped to CastAt — CastSelf
   stays barred via `Decline::SelfRoute`, the CTD-class t6+QNAM cell), cooldown-
   consulted permit (`permitAfter` on the latch), potion-exempt deny (only
   formType==Spell), flicker-proof latch (combat-duration hold), + the INI-gated
   **P1 combat-style probe** (`bProbeCastStyle`, default OFF; CSTY 0x832) to
   de-risk Stage 2. Field test pending (watch items in the table above).
   **Stage 2 (#59, the headline toggle `bFullCastControl`, default OFF) — NEXT,
   IN PROGRESS.** marth green-lit it 2026-08-06 ("just in case people want very
   tight control of casters"), to build right after v1.0.33 deploys. Own every
   decision the AI's cast machinery consults (deny-all + spell-scoring 0x0C +
   CalcCastMagicChance 0x08 + swap combatStyle 0x38 to an MFO caster CSTY) →
   100% vanilla-animated+mobile, MFO owns what/when/whom. **The combatStyle-swap
   leg now rides the PROVEN v1.0.33 rails:** extend the unified CombatStyle
   ownership with an `MFO_CastStyle` stance driven by CAST gambits on the
   UpdateCombat hook, instead of the fragile CheckStartCast-driven probe. Still
   gated on 5 open design questions for marth (force the exact gambit spell vs
   bias-to-cast; caster-CSTY aggressiveness; override target selection too; etc.
   — see task #59).
   **Also folded into this mage update (marth, 2026-08-06):** **#61 fashionrim**
   (armor-only dolls toggle — disables armor looting, the mage school-robe
   loadout, armor auto-equip, MEO gem transfer, armor buy/sell; weapons
   untouched). #61 is specified as an MCM toggle and **depends on #55** (broken
   MCM registration) or it's a dead checkbox — so the mage cut must either fix
   #55 first or ship #61 INI-gated like `bWeaponStyleControl`/`bFullCastControl`.

1. **✅ FIXED in v1.0.34 — #55 MCM new toggles = empty checkboxes on existing
   saves.** Root cause: MFO never shipped `MCM/Config/MFO/settings.ini` (the
   author defaults file MCM Helper REGISTERS from); it shipped only the mutable
   user store, which MO2 shadowed with a stale copy. Now ships the defaults file
   + `audit_mcm.py` release gate (every control wired in all 5 places or the
   build fails). Verified against SkyUI/Precision/TDM/TrueHUD (which ship it).
   Historical detail below.
   #55 — MCM new toggles = empty, unresponsive checkboxes on existing saves.**
   RECURRING ("as per usual"). All file touch points verified correct (atomic,
   parse, ResetToDefaults, kMcmDefaults heal table, config.json control, Settings
   ini, seed ini); live deck store has the keys = 1 under `[General]`; no MCM
   parse error. So it's MCM Helper NOT REGISTERING the new ModSettings for an
   existing save (control draws, nothing bound). `EnsureMcmDefaults` (append to
   store ini at kInputLoaded) is insufficient. FIX: diagnose empirically (diff a
   working installed mod that adds toggles across updates — the "read the data"
   rule) + add a `tools/audit_mcm.py` **release gate** checking every toggle's
   touch-points are consistent, so broken MCM can't ship. marth: "please never
   release broken mcm options."
2. **#48 — retreat didn't relocate; StopCombat fix (v1.0.24) needs validation.**
   Watch `[retreat] moved=` in the next outmatched fight; escalate to a Flee
   package if `moved≈0`.
3. **#24 — retreat threshold tuning** (fire sooner, chase-leash) — pending values
   with marth.
4. **#31 — autonomous town errands** (walk-to-merchant / door nav) — the headline
   NEXT FEATURE; generalises the loot-travel `Packages.*`/`Logistics.*` machinery.
5. **#29 — MEO v1.0.8** finalize (strip `[wdiag]`, bump, release) — *different mod*
   (marth's Equipment Overhaul), same dev flow.

## v1.0.35 — BUILT + DECK-DEPLOYED FOR FIELD TEST, PUBLIC RELEASE HELD

**Deployed to deck 2026-08-06 (DLL d09e1a72, CI run 31137713645). NOT tag-pushed
/ NOT gh-release'd** — this is a high-risk cast-abort hook; the GitHub release
waits on marth's freeze/CTD field-test. To publish once he confirms:
`git push origin v1.0.35` + `gh release create v1.0.35 …` (main agent). Fable-
reviewed twice (2 blockers + majors fixed). **Field-test watch:** no follower
freeze/CTD; `[consent] SpellCast fires on the MAIN/NON-main thread` (one-shot —
tells if friendly-fire is active); `HARD-ABORTED … (exclusive)` in "exact";
`HARD-ABORTED … (friendly fire)` instead of Fireballs-in-the-back; Absorb Health
now obeys "ignore buffs & heals". Design detail below.

1. **HARD cast suppression ("the hard way", marth).** CheckStartCast (0x06) is
   advisory — a denied non-gambit spell still fires (deck: Marcurio cast Icy
   Shard/Turn Undead/Mage Armor while latched in "exact"). FIX: also hook
   `MagicCaster::SpellCast` (VTABLE_ActorMagicCaster[0..2] idx 0x09) — the real
   cast-execution — and ABORT (don't call original) when a latched follower's
   spell is denied. Shared decision with CheckStartCast.
2. **Effect-based spell classification** (replaces the caster-vtable category).
   Classify by the spell's effects: any hostile/detrimental effect → Offense
   (fixes marth's Absorb Health complaint — it was tagged Heal by the Restore
   caster); else beneficial Health effect → Heal (self via Delivery); else Buff.
   `EffectSetting::IsHostile()/IsDetrimental()`, `data.primaryAV==kHealth`.
3. **Friendly-fire hold.** The engine's HasLineOfSight passes THROUGH allies, so
   MFO fired Fireballs into Auri's back in a hallway. FIX: in the SpellCast hook,
   for an OFFENSIVE spell, abort if a teammate (`IsPlayerTeammate`) is within the
   spell's AoE (`GetLargestArea()`) of the target OR on the caster→target line
   (point-near-segment). INI kill-switch `bFriendlyFireHold` default ON.
   (Drop the earlier wrong idea of "ignore allies in the LoS raycast".)

## v1.0.37 — NEXT: serious LINE-OF-SIGHT review (marth flagged 2026-08-06)

Two separate LoS problems, NOT fixed by v1.0.36 (which only fixes which spell is
cast, not aim/LoS):
1. **Shooting walls/floors.** Sightline uses the engine's `Actor::HasLineOfSight`
   (Sightline.cpp:65), which fails-open on "unknown" and only holds on a confirmed
   "occluded" (Actuation.cpp:56-63). An unreliable/unknown verdict lets a forced
   cast fire into terrain. FIX: a real geometry raycast (caster eyes -> target),
   not the engine boolean — treat a solid hit before the target as occluded.
2. **Hitting teammates on occasion.** The friendly-fire hold (v1.0.35, in the
   cast hook) is gated OFF because CheckCast/SpellCast fire on a NON-main thread
   ("[consent] ... fires on a NON-main thread"), so the highActorHandles walk is
   skipped (UAF-safe fail-open). FIX: snapshot teammate positions on the main
   thread (the MainThread pump / evaluator tick) into a lock-free structure the
   off-main hook reads; do the AoE/line check against the snapshot.
Both want a focused review + Fable pass (touches the raycast + threading).

- **BUG: cast-in-logistics never fires out of combat** (marth 2026-08-06, deck).
  Marcurio would not cast Magelight/Candlelight on an "At night" (cond.is_night)
  logistics rule. Verified GOOD: is_night is implemented right (Evaluator.cpp:362,
  Calendar::GetHour 20-6h), is_night IS in kCondsLogi ("At night"), and the cast
  dispatch is wired (Logistics.cpp:3297 -> Actuation::Fire). But the deck log shows
  the logistics cast NEVER attempts (no [cast]/[pkg] for a light spell). CONFIRMED
  the CONDITION is fine: Auri's equip_torch fires on the SAME is_night rule at
  night (deck log), so the bug is 100% the out-of-combat CAST, not is_night. Suspect:
  Actuation::Fire->CastOn's AI-first-grace + force-on-miss (ForceCast/Packages::
  CastSelf) was built for COMBAT and doesn't trigger out of combat (the AI never
  casts -> grace should elapse -> force, but it doesn't). Also check: the cast_self
  buff-active skip (HasMagicEffect) for Candlelight, and whether cast_target
  (Magelight) can resolve a target with no foe. NEXT: add a log line in the
  logistics cast dispatch (rule reached? Fire's Result?) then fix the OOC force path.
- **ENH: add cond.is_dark (ambient light level), better than is_night** (marth):
  a dark dungeon by day needs light; a lit town at night does not. A light-level
  read (interior ambient / GetLightingRun-style) beats the clock. Pairs with the
  cast-in-logistics fix above (the motivating use: auto candlelight/magelight).

## Backlog (captured, not scheduled)

- **TOWN UPDATE (next after the mage cut) — headline #31 autonomous town errands
  + economy gear BUY.** marth 2026-08-06: the mage update is the current cut; the
  next is the Town Update (walk-to-merchant/door nav, #31) and gear-buying at
  vendors folds in naturally (buying IS a town errand).
  - **Economy armor/weapon BUY:** the economy currently BUYS only consumables
    (Logistics `addNeed`: kPotHealth/Stamina/Magicka, kArrows, kBolts); selling
    already covers weapons+armor. Buying is NATIVE-ONLY (Papyrus hands all stock
    to `PlanBuy`) but must buy UPGRADES only, "same restrictions/features as
    looting" (marth) — so factor the loot-upgrade predicates out of the
    monolithic `LootNearby` loop (weapon-class match; `ArmorIsBetter`; the mage
    school-robe path via `MageApparelIsBetter`+school), add NeedCat kWeapon/kArmor
    + `ClassifyBuy` cases, gate armor buy behind #61 `bDollsMode`, and generate
    gear buy-needs from the loot_equipment gambit. Own Fable review + CI (touches
    shared loot code).

- **~~Economy selling socketed items~~ — NON-ISSUE (resolved 2026-08-06).** It was
  marth using a `resurrect` console command, not the economy selling. No bug; the
  log correctly showed zero `[econ]`/sell events. (Kept as a note so it isn't
  re-investigated.)
- **#56 — Combat overlay X/Y position adjuster** (now folded into the mage cut) —
  MCM sliders (or live drag) into the overlay's ImGui window pos (Board.cpp);
  controller nudge per the family rule.
- **#57 — Matching armor sets (MCM toggle, default OFF).** NOT per-piece: a
  set-COLLECTION state machine — the follower collects a better set's pieces
  (holds them, doesn't wear piecemeal, and the economy must NOT sell in-progress
  set pieces) until the set is COMPLETE, then equips it all at once; with a
  piecemeal OVERRIDE when a single piece is a big enough upgrade to wear now.
  Stateful + save-persisted + economy sell-exemption → focused later build.
- **#61 — "fashionrim" / my-followers-are-dolls debug toggle** (rolls into the mage
  update). Default-OFF Debug MCM toggle. **ARMOR ONLY for now:** disables armor
  looting, the mage school-robe loadout, armor auto-equip, MEO gem transfer on
  armor swaps, and armor buy/sell (partial). WEAPONS unaffected — weapon
  switching, weapon looting, ammo, weapon trade all still work. Warnings cover the
  armor breakage. Depends on #55 (MCM registration fix) or it's a dead checkbox.
- **#58 — Recognize framework followers (Kaidan, Inigo, Lucien).** Custom-framework
  voiced followers with bespoke quest/AI systems, not the vanilla follower faction
  path MFO detects. Reverse-engineer each one's active-follower signal from its
  installed ESP + Papyrus scripts (read the source — they expose no API), assess
  gambit/AI interop, document the mechanism in ENGINE_NOTES. Verify which are in
  marth's modlist first (primary sources).

## Where knowledge goes

See INDEX.md's routing table. Durable facts about *this session's* mechanisms are
already in the memory store (furniture latch, cast spell-choice, LoS worker-safe
pattern, MCM schema, release scope/publish hygiene). Engine findings → ENGINE_NOTES
§0; incident-born rules → INVARIANTS; symptom→cause→fix → DEBUGGING.
