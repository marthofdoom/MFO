#pragma once
#include "PCH.h"

// INI + MCM Helper settings. DESIGN.md §7.
//
// Two files, LAST WINS:
//   Data/SKSE/Plugins/MFO.ini     (dev/seed defaults)
//   Data/MCM/Settings/MFO.ini     (MCM Helper's store -- lands in MO2 overwrite
//                                  and SURVIVES mod updates)
//
// The rules here are all scar tissue (INVARIANTS #37-#39):
//   - A key whose SEMANTICS change must be RENAMED. MCM Helper persists by key
//     name, so a stale value gets silently reinterpreted -- MEO cut an XP
//     stream ~100x this way and found the stale number live in a deployed
//     profile.
//   - RESET-then-parse every pass, so an ABSENT key reverts to default rather
//     than sticking at its last in-memory value.
//   - Skip unparseable values; NEVER let a failed parse become 0.0.
//   - Strip MCM Helper's UTF-8 BOM.
//
// Every value a live re-read can change is an ATOMIC (INVARIANTS #7).

namespace MFO::Config {

    // -- detection -----------------------------------------------------------
    inline std::atomic<bool>  g_allowSummons{ false };

    // -- rapport (BALANCE.md §5) --------------------------------------------
    inline std::atomic<float> g_rapportRate{ 1.0f };
    inline std::atomic<float> g_rapportKill{ 1.0f };
    inline std::atomic<float> g_rapportBossMult{ 5.0f };
    inline std::atomic<float> g_rapportDragonMult{ 10.0f };
    inline std::atomic<float> g_sharedRadius{ 3000.0f };
    // Levels ABOVE the player at which a kill counts as a boss. IsUnique()
    // alone missed generic dungeon bosses (a bandit chief with a boss bar).
    inline std::atomic<int>   g_bossLevelDelta{ 5 };

    inline std::atomic<int>   g_rank2{ 250 };
    inline std::atomic<int>   g_rank3{ 1000 };
    inline std::atomic<int>   g_rank4{ 2500 };
    inline std::atomic<int>   g_rank5{ 5000 };

    // Rank-up HUD toast (flair #13): one DebugNotification when a follower's
    // rapport RANK changes -- "Lydia fights beside you with new resolve."
    // Transition-only by construction (thresholds make rank changes rare), ON
    // by default: it is feedback on something that already happened, never an
    // engine act on the follower. bRapportToasts.
    inline std::atomic<bool>  g_rapportToasts{ true };

    // -- diagnostics ---------------------------------------------------------
    inline std::atomic<bool>  g_enableLogging{ true };
    inline std::atomic<bool>  g_profileRapport{ false };
    // Board skin 0..3 (DESIGN §6.7a): Ebony & Brass / Dwemer Parchment /
    // Soul Cairn / Quicksilver. Live, MCM dropdown.
    inline std::atomic<int>   g_menuStyle{ 0 };
    inline std::atomic<bool>  g_showHud{ false };

    // Writes test gambits onto a player-keyed record so the co-save round-trip
    // is provable without a UI. DEFAULT OFF: it is only meaningful if you
    // intend to SAVE with the mod active, and until that is on the table it
    // just puts a synthetic record in the Field Kit.
    //
    // An INI key rather than the compile-time constant INVARIANTS #37 would
    // normally ask for, because there is no local compiler here -- flipping a
    // constant costs a CI round-trip, flipping a key costs nothing.
    inline std::atomic<bool>  g_seedTestData{ false };

    // How long after a follower was last SEEN fighting they still count as
    // having shared a kill. A RAPPORT setting -- it covers the queued-task gap:
    // the fight is already over by the time a death event is processed (#51).
    inline std::atomic<float> g_sharedCombatGrace{ 15.0f };

    // Make the FOLLOWER cast (equip the spell, real animation) instead of
    // applying the effect silently. DESIGN §4.5b.
    // PROBE ONLY, DEFAULT OFF. DoCombatSpellApply turned out NOT to be a
    // commanded animated cast -- Actor.psc's own comment is "Apply a spell to a
    // target in combat", and every shipped call site uses it as an instant
    // silent apply (Bethesda's own Dawnguard shield script among them). It is
    // the Papyrus twin of CastSpellImmediate. Kept behind a flag only to
    // measure whether it deducts magicka; it is NOT the animation answer.
    inline std::atomic<bool>  g_commandCast{ false };
    // THE ATTACK VERB (ENGINE_NOTES §0.14). Installs a vfunc hook. Now ON by
    // default: it has been field-proven (the flagship Attack gambit no-ops
    // without it -- audit 2026-07-28), so #45's probe gate has been cleared.
    inline std::atomic<bool>  g_commandTarget{ true };
    // DIK code for the focus hotkey. 0x2B is backslash -- unbound in vanilla
    // Skyrim, so it will not fight an existing control. 0 disables.
    inline std::atomic<int>   g_focusKey{ 0x2B };
    // Put the gambit spell in the follower's hand so they cast it with a real
    // animation, and -- crucially -- so Loadout::Prepare's HasSpell/affordability
    // gate runs (the silent path skips it and would cast an unknown spell). Now
    // ON by default: this is the honest cast path, and its competence gate is
    // required by DESIGN §5.4 / INVARIANTS #20 (audit 2026-07-28).
    inline std::atomic<bool>  g_equipToCast{ true };
    // How long MFO waits, after putting a spell in a follower's hand, for their
    // OWN AI to cast it before casting silently instead. Zero would recreate
    // the confound this exists to prevent.
    inline std::atomic<float> g_aiCastGrace{ 3.0f };
    // Fraction of magicka a gambit will not spend below. OFF by default, and
    // that default is deliberate: a follower healing to stay alive is EXACTLY
    // the case where a floor gets them killed, holding a spell they are not
    // allowed to cast. It exists for players who want casters to keep something
    // in reserve for utility, not as a rate limiter.
    inline std::atomic<float> g_magickaReserve{ 0.0f };

    // Minimum seconds between gambit casts for one follower. THE RATE LIMITER.
    //
    // MFO cannot tell a combat AI to cast less often -- but it decides what is
    // in the follower's hand, and they can only cast what they hold. So a cast
    // takes the spell back and the cooldown decides when it returns. Paces the
    // AI and MFO's own fallback with one number.
    //
    // Not a resource cap: a weak heal SHOULD be cast many times over a long
    // fight. What looked wrong in the field was the interval, not the total.
    inline std::atomic<float> g_castCooldown{ 4.0f };

    // AUTO fan-out DoT recast threshold (fix #4). For a HOSTILE duration spell
    // already ticking on a foe, MFO does not blanket-skip: it decomposes the
    // spell into instant BURST damage vs per-second DOT and recasts when
    //   burst >= dotRate * timeRemaining * fDotRecastBurstRatio.
    // 1.0 = recast once the up-front burst outweighs the DoT still to be
    // delivered; >1 waits longer (favours letting the tail run), <1 recasts
    // more eagerly. Pure-burst spells always fire; pure-DoT spells wait.
    inline std::atomic<float> g_dotRecastBurstRatio{ 1.0f };

    // BENEFICIAL DURATION recast suppression (fix #3/#6). A beneficial DURATION
    // buff (Candlelight/Magelight and other lights, wards, fortifies) must NOT be
    // re-cast until it is near real expiry -- the field bug was Magelight
    // respamming every ~2 s while magicka lasted (a light applied via
    // CastSpellImmediate never registers HasMagicEffect, so the already-active
    // gate never caught it). MFO suppresses re-firing that spell on that caster
    // until the elapsed time reaches the spell's own authored duration x a
    // JITTERED fraction, so the recast beat lands near true expiry AND looks
    // human (never a fixed cadence). INSTANT beneficial spells (duration 0, e.g.
    // instant heals) are EXEMPT -- they legitimately re-fire on demand.
    //   window = authoredDuration * fBeneficialRecastFrac * (1 +/- jitter),
    //   jitter uniform in [-fBeneficialRecastJitter, +fBeneficialRecastJitter].
    // 0.85 fires the recast at ~85% of the buff's life (a small safety margin so
    // it never lapses); the +/-20% jitter breaks the robotic cadence. Additive to
    // fCastCooldown, not a replacement.
    inline std::atomic<float> g_beneficialRecastFrac{ 0.85f };
    inline std::atomic<float> g_beneficialRecastJitter{ 0.20f };

    // PACKAGE ACTUATION (M9, DESIGN §4.5c). Fill MFO_CommandQuest's alias 0
    // with a follower so the engine instances MFO_CastPackage onto them --
    // vanilla's own follower mechanism, pointed at one action.
    //
    // NOW DEFAULT ON (v1.0.27): the mechanism #45 demanded be measured first
    // HAS been -- every target/castingType/delivery axis is field-proven
    // (§0.21-0.23), and it is the delivery route for the hybrid forced cast
    // below, which is inert without it. The save-serialization risk that kept
    // it off is now owned end-to-end: release is by marker eviction (never the
    // player, #73) and ReleaseAll's load sweep evicts a mid-cast save's latch
    // with a measured VerifyDetachedFrom readback. Neither shipped INI pins
    // this key, so the default is what runs.
    inline std::atomic<bool>  g_usePackages{ true };

    // THE HYBRID FORCED CAST (v1.0.27, the Marcurio/Firebolt fix). AI-first
    // stays: equip + consent + fAiCastGrace, the mobile animated path. But
    // when the AI MISSES -- casts a DIFFERENT spell during the grace (which
    // also resets the grace clock forever, so the miss was self-concealing),
    // or lets the grace elapse -- the CONFIGURED spell is forced through the
    // proven cast package at the rule's chosen target. ON by default: this is
    // what makes "cast Firebolt" mean Firebolt. The forced shot additionally
    // requires line of sight (Sightline) so it never fires into a wall.
    inline std::atomic<bool>  g_forceCastOnMiss{ true };

    // INFLUENCE actuator (§0.28). Hook CheckStartCast so the follower's own
    // combat AI casts the gambit spell while staying mobile. Installs a vfunc
    // hook. Now ON by default (field-proven; audit 2026-07-28).
    inline std::atomic<bool>  g_casterHook{ true };
    // 0 = LOG only (observe the AI's answer, change nothing -- the read-only
    // experiment). 1 = FORCE (override a veto to YES when latched). Ships in
    // FORCE: LOG changes nothing, so the mod would not cast in LOG mode.
    inline std::atomic<int>   g_casterMode{ 1 };

    // PROBE, default off. Drive the MagicCaster state machine by hand instead
    // of applying the effect: SetCurrentSpell + desiredTarget + RequestCastImpl,
    // then let the engine advance it. marth's principle -- if the game code can
    // trigger a real cast, so can we; the question is which call the AI makes.
    inline std::atomic<bool>  g_driveCaster{ false };

    // P1 PROBE (v1.0.32), DEFAULT OFF -- dev opt-in ONLY, INI-only (no MCM;
    // it is an experiment, not a setting). While a cast rule is latched, the
    // consent hook swaps the follower's LIVE CombatController::combatStyle
    // (per-combat instance, offset 0x38 -- below the §0.29 AE divergence;
    // never the base record) to MFO_CastStyle, a caster-forward CSTY, and
    // restores it on latch release / combat end. Measures the single unknown
    // full cast control depends on: does a magic-inclined style make his own
    // AI cast MORE, and mobile (§0.28's melee bias)? Carries CTD risk by
    // nature of touching a live combat structure -- it must never run unless
    // marth armed it: bProbeCastStyle = 1 in Data/SKSE/Plugins/MFO.ini (the
    // seed file; sections are cosmetic to the parser), then a full restart.
    inline std::atomic<bool>  g_probeCastStyle{ false };

    // FORCED SELF-CAST GATE (Docs/SPEC-self-cast-forced.md), DEFAULT OFF --
    // dev/opt-in ONLY, INI-only (the bProbeCastStyle precedent). Arms the
    // dedicated no-QNAM targType-6 self package (MFO_CastPackageSelf, command-
    // quest alias 2): with it OFF, Packages::Begin's self route stays BARRED
    // (Decline::SelfRoute) exactly as it shipped, and an authored act.cast_self
    // gambit falls through to the silent legacy apply. With it ON, a cast_self
    // gambit -- concentration self-heal/self-ward OR fire-and-forget -- is
    // FORCED through the self package, bounded by the same CastHold discipline
    // as the target stream. §0.22 proved the record shape casts cleanly and
    // REVOKED #67; the gate exists because the PRODUCTION path (arbitrary self
    // spell, in combat, via alias fill/evict) is not yet deck-confirmed -- it
    // must not ship active until the isolation probe passes. bCastSelf = 1 in
    // Data/SKSE/Plugins/MFO.ini, then a restart.
    inline std::atomic<bool>  g_castSelf{ false };

    // FOLLOWER-PROGRESSION SINKER PROBE (design doc §13 P1/P2/P3), DEFAULT
    // OFF — dev opt-in ONLY, INI-only (an experiment, not a setting; the
    // bProbeCastStyle precedent). Arms ProgProbe: a LOG-ONLY throwaway that
    // adds one real perk to a follower's BASE via the SPID pattern, writes a
    // known base-AV allocation, and verifies the guarded reapply at
    // kPostLoadGame. Mutates one perk + one skill on ONE follower when the
    // hotkey is pressed — nothing is serialized by MFO, but the actor's AV
    // write lands in any save taken after. bProgProbe = 1 in
    // Data/SKSE/Plugins/MFO.ini, then trigger with iProgProbeKey below.
    inline std::atomic<bool>  g_progProbe{ false };
    // DIK code for the probe trigger. 0x27 is semicolon — unbound in vanilla
    // (the focus key holds 0x2B backslash). 0 disables the trigger.
    inline std::atomic<int>   g_progProbeKey{ 0x27 };

    // PROGRESSION CATALOG DUMP (component 1 of the ESL addon), DEFAULT OFF —
    // dev opt-in ONLY, INI-only (a verification aid, not a setting; the
    // bProgProbe precedent). The production catalog reader (Progression.cpp)
    // only builds when MFO_Progression.esl is detected — which does not exist
    // yet — so this FORCES the build at kDataLoaded and emits the [prog]
    // census (per-skill node/filter counts, every filtered perk + reason,
    // spot-check nodes) for on-deck verification of the reader against the
    // real merged trees. Read-only over the load order: builds an in-memory
    // catalog and logs — mutates no actor, form, or save. bProgCatalogDump = 1
    // in Data/SKSE/Plugins/MFO.ini, then a restart (read at kDataLoaded).
    inline std::atomic<bool>  g_progCatalogDump{ false };

    // PROGRESSION DEV HARNESS (component 2 of the ESL addon), DEFAULT OFF —
    // dev opt-in ONLY, INI-only (the bProgProbe precedent). One hotkey that
    // exercises the ALLOCATOR on the current follower before the board (comp
    // 3) exists; the verb comes from the addon's MFOP_DevCmd GLOB, set from
    // the console (`set MFOP_DevCmd to N`): 0=status 1=enroll 2=cycle-class
    // 3=skills 4=alloc-next-perk 5=respec 6=economy. Inert without
    // MFO_Progression.esl (the allocator is). bProgHarness = 1 in
    // Data/SKSE/Plugins/MFO.ini, trigger with iProgHarnessKey below.
    inline std::atomic<bool>  g_progHarness{ false };
    // DIK code for the harness trigger. 0x28 is apostrophe — unbound in
    // vanilla (focus holds 0x2B backslash, the probe 0x27 semicolon).
    // 0 disables the trigger.
    inline std::atomic<int>   g_progHarnessKey{ 0x28 };

    // SHARED GROWTH (progression addon, §15 — a PLAYER setting, so it lives
    // here and later in the MCM Addons section, never in the ESL). Default ON:
    // benched/retained enrollees level at the ESL's divisor rate (half by
    // default). OFF: they match the player's level at full rate.
    inline std::atomic<bool>  g_sharedGrowth{ true };

    // #56 COMBAT-OVERLAY POSITION (mage update). The compact combat HUD
    // (Board::DrawHud) is pinned to the top-right with a top-right pivot; these
    // are its margins in PIXELS from the right edge (X) and the top (Y), so a
    // player can move it clear of another mod's HUD. Default 12/12 = unchanged.
    inline std::atomic<int>   g_overlayX{ 12 };
    inline std::atomic<int>   g_overlayY{ 12 };

    // #61 FASHIONRIM / "my followers are dolls" (mage update). Default OFF.
    // ARMOR-ONLY debug toggle: when on, MFO stops ARMOR ACQUISITION + fitting --
    // armor looting, the mage school-robe loadout, armor auto-equip, MEO gem
    // transfer on armor swaps, and armor BUYING -- so the player dresses
    // followers by hand. SELLING is unaffected (marth: skip acquisition only;
    // worn + MEO-socketed gear is never sold anyway). WEAPONS untouched entirely.
    inline std::atomic<bool>  g_dollsMode{ false };

    // CAST CONTROL SLIDER (mage update). How tightly a follower's OWN spell
    // picks are overridden toward the gambit's spell, graduated (marth):
    //   0 off              -- no control (observe only)
    //   1 ignore buffs+heals -- MFO forces the gambit for OFFENSE; AI keeps buffs+heals
    //   2 ignore heals (DEFAULT/center) -- MFO takes offense+buffs; AI keeps heals
    //   3 ignore self-heals -- MFO takes offense+buffs+heal-others; AI keeps self-heals
    //   4 exact            -- only the gambit spell is ever cast (deny all else)
    // "ignore X" = MFO EXEMPTS category X, leaving it to the AI; tighter toward
    // exact. Applied in the CheckStartCast consent hook (CasterConsent).
    inline std::atomic<int>   g_castControl{ 2 };

    // FRIENDLY-FIRE HOLD (v1.0.35), DEFAULT ON. In the SpellCast hook, hold a
    // follower's OFFENSIVE cast when one of the player's own teammates is in the
    // AoE blast at the target or on the caster->target projectile line -- the
    // engine's HasLineOfSight passes through allies, so without this a mage
    // fires into a teammate's back in a hallway. INI kill-switch (no MCM yet).
    inline std::atomic<bool>  g_friendlyFireHold{ true };

    // #63 QUASH ALLY COMBAT, DEFAULT ON. Followers must never fight EACH OTHER.
    // Friendly fire (a forced offensive cast catching a teammate) can aggro one
    // follower onto another; the built-in friendly-fire hold above is inert on
    // this runtime (its hook runs off-main), so rather than only PREVENT the
    // induction we DETECT AND QUASH the result: whenever a player teammate enters
    // combat with -- or is actively targeting -- another player teammate, MFO ends
    // that combat on both sides (StopCombat). Event-driven (TESCombatEvent, catches
    // the start) plus a per-follower tick backstop (catches crossfire when both
    // were already fighting). Self-heals regardless of what induced the aggro.
    // INI kill-switch bQuashAllyCombat. Watch "[peace]" in MFO.log.
    inline std::atomic<bool>  g_quashAllyCombat{ true };

    // WEAPON-STANCE OWNERSHIP (v1.0.33), DEFAULT ON -- a standard feature, not a
    // probe, so no MCM toggle (#55); INI-only as a debug kill-switch. When an
    // equip gambit wins a follower's hand, the DLL swaps their LIVE
    // CombatController::combatStyle (0x38 -- below the §0.29 AE divergence,
    // never the base record) to a stance-matched MFO CSTY, applied on the
    // combat thread from the UpdateCombat hook. Stops the engine re-drawing the
    // weapon MFO didn't pick (the Auri bow<->mace ping-pong). Reverts with the
    // per-combat controller at battle end. Set bWeaponStyleControl = 0 in
    // Data/SKSE/Plugins/MFO.ini to disable. See CombatStyle.cpp.
    inline std::atomic<bool>  g_weaponStyleControl{ true };
    // Seconds a two-handed wielder must go between weapon swaps. The off-hand
    // swap is free and ungated; stowing a greatsword is not.
    inline std::atomic<float> g_twoHandedDebounce{ 6.0f };

    // -- evaluator (M5) ------------------------------------------------------
    inline std::atomic<bool>  g_seedEvaluatorRules{ false };
    inline std::atomic<bool>  g_profileEvaluator{ false };

    // DEBUG: unlock every gambit slot regardless of rank/rapport, so all
    // gambits are assignable without grinding. SlotsForRank returns the Rank V
    // allowance when set. Default off.
    inline std::atomic<bool>  g_debugUnlockSlots{ false };
    inline std::atomic<float> g_suppressWindow{ 1.5f };
    // -- logistics (DESIGN §4.8) ---------------------------------------------
    // The non-combat table (drink/loot/restock). Now ON by default: a follower
    // who tidies up after a fight is core to the mod feeling like a real person,
    // and every action is gated by ownership, locks and the first-dibs delay
    // below (audit 2026-07-28).
    inline std::atomic<bool>  g_logistics{ true };
    // FIRST DIBS BY DELAY (#22h). A corpse/container is not eligible for
    // follower looting until it has been in the follower's consideration radius
    // this many seconds -- long enough that a player who wants the good sword
    // has walked over and taken it. 4s, not 25: the clock only ticks OUT of
    // combat (logistics runs post-fight), so 25 meant the player had walked away
    // and looting looked dead. 4s reads as "the follower tidies up once the
    // fight is over" while still leaving the player first pick. (DESIGN §4.8.3
    // names this fLootDelaySeconds; the shipped key is fFirstDibsDelay.)
    inline std::atomic<float> g_firstDibsDelay{ 4.0f };
    // THE WAIVER, collapsed but NEVER zeroed (#22h). Once the player takes from
    // a ref its delay drops to this -- not to 0, because QuickLoot IE (Nexus
    // 181813, in 4 of 5 lists here) takes items ONE AT A TIME over several
    // seconds, and the waiver timer RESETS on every take, so the follower moves
    // in this many seconds after the player's LAST take, not their first.
    inline std::atomic<float> g_quickLootWaiver{ 4.0f };

    // ── CLAIM-AND-RELEASE loot priority (dibs redesign; Fable + marth 2026-07-30)
    // The follower may take a value-tier from a source only once the PLAYER's
    // claim on it RELEASES -- driven by evidence about the player (proximity,
    // facing, a take, departure), not a wall clock. These are the tuning knobs;
    // the model that reads them lands with the Logistics rewrite. Defaults are
    // Fable's. When the model lands, g_firstDibsDelay/g_quickLootWaiver above are
    // renamed g_gearGrace/g_rejectionLinger (same meaning).
    // (v1 tiers by CATEGORY: gold = Valuable, equipment = Gear. Treating
    //  high-VALUE gear as a Valuable too -- an fValuableThreshold with a per-item
    //  value peek -- is a ROADMAP refinement, not wired here, so no dead knob.)
    inline std::atomic<float> g_chanceRadius{ 512.0f };       // player "had a chance" within this of the source
    inline std::atomic<float> g_fairChance{ 6.0f };           // seconds near+facing before valuables release
    inline std::atomic<float> g_abandonDelay{ 45.0f };        // never-approached backstop before valuables release
    inline std::atomic<float> g_departRadius{ 700.0f };       // player must leave this far for "moved on"
    inline std::atomic<float> g_playerBubble{ 256.0f };       // convergence yield: defer within this of a target

    // How far a follower REACHES for loot. Until active pathing lands (the
    // deferred "Option A"), this is a TELEPORT-GRAB radius: the follower takes
    // from any eligible corpse/container within it. At arm's-reach (200u)
    // looting almost never fired -- a corpse is rarely that close once a fight
    // ends (marth, deck test 2026-07-29). Raised, and made a KEY so it is
    // tunable by editing the synced INI with no rebuild. The parent-cell walk
    // (§0.30) still iterates only the follower's OWN cell, so values past one
    // cell (~4096u) gain nothing real. fLootRadius.
    inline std::atomic<float> g_lootRadius{ 3000.0f };   // ~4-5 rooms (1 unit ~= 1.4cm; cell = 4096)

    // OPTION A -- engine-pathed loot: the follower WALKS to the loot instead of
    // teleport-grabbing it. DEFAULT ON. The v0.8.1 strand (unreachable targets,
    // a release that never fired, churn) is fixed in v0.8.2: a NATIVE alias clear
    // (ForceRefTo None -- the VM Clear failed because MFO's aliases carry no
    // script), a short WALKABLE radius (g_travelRadius, not fLootRadius) with a
    // distance-scaled deadline, closest-first, and a travel-failed skip so an
    // unreachable target is not re-tried into a loop.
    inline std::atomic<bool>  g_lootTravel{ true };

    // LOOT IN PLAYER HOMES. Default OFF: a follower rifling your own house reads
    // as theft, not battlefield tidying, so looting is suppressed in any cell
    // whose LOCATION carries the vanilla LocTypePlayerHouse keyword (bought
    // houses, Hearthfire builds, and the home mods that set it). Turn ON to let
    // them loot everywhere. Only LOOTING is gated -- drinking/supply still run.
    // bLootInPlayerHomes.
    inline std::atomic<bool>  g_lootInPlayerHomes{ false };

    // #21 FOLLOWER ECONOMY. Default OFF. When on, a follower standing at a merchant
    // sells its junk (Phase 2) and buys supplies it's short on (Phase 3), via the
    // MFO_Trade Papyrus bridge. OFF makes the econ scan a DRY RUN -- it logs the
    // plan ([econ] WOULD sell...) but mutates nothing, so it's safe to leave the
    // scan enabled while tuning. bEconomy.
    inline std::atomic<bool>  g_economy{ false };

    // AUTO-RETREAT (combat-sense 3, the outnumbered DEFAULT rule). ON by default
    // (marth): a follower who is badly outnumbered/hurt (confidence below the 0.25
    // floor -- foe count feeds confidence) AND far from you in combat falls back to
    // your side under kIgnoreCombat travel. This is the one place MFO acts without
    // an authored rule; the MCM toggle is the override (turn OFF to require an
    // authored fall-back rule instead). Fires once per fight. bAutoRetreat.
    inline std::atomic<bool>  g_autoRetreat{ true };

    // MAGIC LOADOUT (v1.0.29). A magic-user follower -- anyone with at least
    // one ENABLED cast gambit; role is GAMBIT-DRIVEN here, never skill-guessed
    // -- loots the gear a mage actually wants: enchanted apparel that boosts
    // his most-cast school (a Destruction robe for a destruction mage, judged
    // by school match then fanciness, never by the armor-rating gate that
    // can't see rating-0 clothing), plus ONE one-handed melee backup for when
    // his magicka runs dry -- vanilla AI draws a weapon at zero magicka, and a
    // caster with nothing to draw just swings fists. Master switch for both;
    // OFF restores the old rating-only loot for everyone. bMagicLoadout.
    inline std::atomic<bool>  g_magicLoadout{ true };
    // The mage backup is DAGGERS ONLY by default (a dagger reads as a caster's
    // sidearm; a looted war axe turns him into a bad warrior). OFF opens the
    // backup to the best of ANY one-handed weapon. bMageDaggersOnly.
    inline std::atomic<bool>  g_mageDaggersOnly{ true };

    // #62 BEAST-RACE HEAD FIX, DEFAULT ON -- a debug kill-switch (INI-only, like
    // bWeaponStyleControl). Beast-race followers (Khajiit/Argonian, incl. custom
    // ones like Inigo) go HEADLESS when MFO equips looted gear on them: beast head
    // parts attach during the full 3D build, not during a scripted armor-addon
    // swap, so the head is left detached. When ON, MFO forces a 3D rebuild
    // (Actor::DoReset3D) on the MAIN thread right after equipping on a beast-race
    // follower, which reattaches the head. Beast-gated so human followers never
    // eat the rebuild. Set bBeastHeadFix = 0 in Data/SKSE/Plugins/MFO.ini to
    // disable (e.g. if the rebuild's 1-frame refresh is unwanted). See Logistics.cpp.
    inline std::atomic<bool>  g_beastHeadFix{ true };

    // LOW-POWER POTION FLOOR (marth: "ignore low power potions entirely, loot
    // strongest to weakest"). A restore potion whose magnitude is below this is
    // never looted; the rest are taken strongest-first. 0 = AUTO -- use the floor
    // derived from the load order's weakest restore tier (Logistics::
    // ComputeWeakPotionFloor). A positive value is an explicit magnitude floor
    // across all restore types. Fortify/cure potions are never filtered. iMinPotionMag.
    inline std::atomic<int>   g_minPotionMag{ 0 };

    // HARD CEILING on the walk-to-loot distance (units). The real limit is the
    // confidence LEASH (walkLimit = min(leash, this)); this is only for anyone
    // who wants to clamp the walk BELOW the leash. Default high so the leash
    // rules -- the old fixed 768 hid most in-leash bodies (deck: 6 eligible, only
    // the 2 inside 768 ever attempted). fTravelRadius.
    inline std::atomic<float> g_travelRadius{ 4096.0f };

    // LOOSE-ITEM PICKUP DISTANCE (fLooseAcquireDist). A loose world item (gold/
    // ammo pile) is acquired by dispatching ObjectReference.Activate, which the
    // ENGINE runs regardless of physical reach -- so a follower who walked as
    // close as the navmesh allows and STALLED short of a corpse's arm's-reach
    // (kArrivalDist 160) still grabs it. Bigger than kArrivalDist because the
    // navmesh often ends well off a pile sitting on a table/shelf (marth field
    // report: Xelzaz stalled ~221 from reachable gold and never triggered the
    // grab). Corpses still need real arm's reach (a transfer, not an Activate),
    // so this is LOOSE-only. Tune up if piles beyond this still get skipped.
    inline std::atomic<float> g_looseAcquireDist{ 300.0f };

    // BATCH EXCURSION (walk-to-loot batching). A follower stays CLAIMED (loot
    // quest at priority 60) across multiple corpses instead of returning to the
    // player between each, retargeting closest-first. Two bounds:
    //  - fBatchLinger: when no walkable loot remains but some is still under the
    //    player's dibs, HOLD in place this long (re-scanning) before giving up
    //    and returning -- lets a just-released corpse extend the batch. 0 = no
    //    hold (pure exhaust-and-return). fBatchLinger.
    //  - fExcursionMax: whole-excursion hard cap (seconds). The follower returns
    //    to the player after this no matter what, so a batch can't run away.
    // (No batch-size/leg count and no separate "zone" radius: the confidence
    // leash IS the zone, and a time cap subsumes a leg cap -- fewer knobs.)
    inline std::atomic<float> g_batchLinger{ 4.0f };
    inline std::atomic<float> g_excursionMax{ 60.0f };

    // WALK-TO-LOOT GAIT (task 16, ENGINE_NOTES §0.35). The travel package's
    // PKDT byte 6: 0=Walk, 1=Jog, 2=Run, 3=FastWalk. Meaningful ONLY because
    // the ESP ships the 0x2000 Preferred-Speed-enable flag (v0.8.3) -- without
    // it the byte is inert (measured: 4,386/4,502 flagless vanilla PACKs carry
    // an inert 2). Applied to the live TESPackage record by Gait::Apply(),
    // which runs at the end of every Config::Read() -- NOT read per-tick, so
    // it is derived engine state, not a live atomic. Default 2 (Run) = the
    // shipped ESP byte, so an absent key changes nothing. iTravelGait.
    inline std::atomic<int>   g_travelGait{ 2 };

    // NAVMESH GATE (units). Before dispatching a walk, MFO checks the nearest
    // navmesh vertex to the loot ref; if it's farther than this, the ref is
    // OFF the navmesh (a corpse clipped into geometry / on furniture) and the
    // Travel package could never path to it -- skip it before he freezes. 300
    // ~= kArrivalDist + half a large floor triangle edge, since nearest-VERTEX
    // overestimates nearest-surface. Raise if reachable bodies get skipped.
    inline std::atomic<float> g_navmeshGate{ 300.0f };

    // THE CONFIDENCE LEASH (DESIGN core tenet). How far from the PLAYER a
    // follower will range to engage/loot is not fixed -- it scales with the
    // follower's confidence to survive alone (Confidence::Of). Bold in an easy
    // fight (leash -> max, pushes ahead), cautious in a hard one (leash -> min,
    // falls back to the player). These bound that leash, in game units.
    // Units: 1 ~= 1.4cm, ~70/m; a room ~600-1000, an exterior cell 4096. So the
    // scared floor is ~one room and the confident ceiling is several rooms.
    inline std::atomic<float> g_leashMin{ 512.0f };    // scared: ~one room from the player
    inline std::atomic<float> g_leashMax{ 4000.0f };   // confident: several rooms, near a full cell
    // Combat chase radius (#22): how far from HIMSELF a follower engages a foe,
    // confidence-scaled. Floor well above melee so he always fights an adjacent
    // foe; ceiling lets a bold follower range the field. Stops the distance-blind
    // "attack the weakest" from marching him across a pack.
    inline std::atomic<float> g_chaseMin{ 600.0f };    // scared: only what's on top of him
    inline std::atomic<float> g_chaseMax{ 3000.0f };   // confident: ranges across the field

    // Melee reach -- the power-attack RANGE GATE. How close a follower must be to
    // the CHOSEN foe before the "-> Power attack" action actually swings. A power
    // attack is a melee lunge, so firing "attackPowerStartInPlace" from range just
    // swings at air (marth, field: the follower power-attacked whenever ANY foe
    // blocked, at whatever distance). The action LATCHES the foe and the engine's
    // own combat AI walks him in (same approach plain Attack relies on); the anim
    // fires only once GetDistance(follower, foe) <= this. Two adjacent humanoids
    // read ~80-130u origin-to-origin, so ~200u leaves margin for the lunge without
    // swinging at a foe a room away -- matches the authored melee thresholds
    // (GAMBIT_FLOWS §3.2/3.3, 150/200u). fMeleeReach -- tunable by editing the
    // synced INI with no rebuild.
    inline std::atomic<float> g_meleeReach{ 200.0f };

    // Which caster a gambit spell goes through. PROVEN 2026-07-21: NO source
    // animates -- kLeftHand/kRightHand/kOther were all tried in the field and
    // behave like kInstant (ENGINE_NOTES §0.10). Kept configurable because it
    // costs nothing and the next engine question may need it; defaulted back to
    // 3 (kInstant), which at least names what actually happens.
    inline std::atomic<int>   g_castSource{ 3 };


    // Reset-then-parse both files, seed then MCM. Safe to call repeatedly.
    // Called at kDataLoaded AND on Journal-Menu (MCM) close -- Diagnostics'
    // MenuSink re-reads so MCM edits apply on closing the menu, not at next
    // load. NOTE: settings read live from these atomics take effect
    // immediately; a setting mirrored into derived UI state (g_showHud ->
    // Board's g_hud) must be RE-APPLIED after Read() by whoever re-reads.
    void Read();

    // MCM migration self-heal: ensure the MCM Helper store (Data/MCM/Settings/
    // MFO.ini) has every ModSetting key, appending any a mid-playthrough update
    // added so its toggle binds on an EXISTING save (MCM Helper does not seed
    // missing keys). Call ONCE at kDataLoaded, BEFORE Config::Read and before MCM
    // registration. Idempotent + degrades safely; see the .cpp for the key table.
    void EnsureMcmDefaults();

}
