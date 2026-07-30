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

    // PACKAGE ACTUATION (M9, DESIGN §4.5c). Fill MFO_CommandQuest's alias 0
    // with a follower so the engine instances MFO_CastPackage onto them --
    // vanilla's own follower mechanism, pointed at one action.
    //
    // DEFAULT OFF, and #45 is the reason: this is one new engine mechanism,
    // and it is not trusted until it is measured. It also writes state the
    // engine SERIALIZES INTO THE SAVE (an alias fill), which is a strictly
    // higher class of risk than anything else behind these flags -- a bug here
    // outlives the session rather than ending with it.
    inline std::atomic<bool>  g_usePackages{ false };

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

    // How far a follower will WALK to loot (units), separate from the scan
    // radius. Must be genuinely reachable within the travel deadline -- keep it
    // near one room, not the whole scan. Loot beyond it is left (the follower
    // grabs it later when following brings them closer -- "loot with the player's
    // movement"), which is also fairer to the player. fTravelRadius.
    inline std::atomic<float> g_travelRadius{ 768.0f };

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

    // THE CONFIDENCE LEASH (DESIGN core tenet). How far from the PLAYER a
    // follower will range to engage/loot is not fixed -- it scales with the
    // follower's confidence to survive alone (Confidence::Of). Bold in an easy
    // fight (leash -> max, pushes ahead), cautious in a hard one (leash -> min,
    // falls back to the player). These bound that leash, in game units.
    // Units: 1 ~= 1.4cm, ~70/m; a room ~600-1000, an exterior cell 4096. So the
    // scared floor is ~one room and the confident ceiling is several rooms.
    inline std::atomic<float> g_leashMin{ 512.0f };    // scared: ~one room from the player
    inline std::atomic<float> g_leashMax{ 4000.0f };   // confident: several rooms, near a full cell

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

}
