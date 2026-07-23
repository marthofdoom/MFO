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
    inline std::atomic<float> g_rapportSurvival{ 1.0f };
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
    // THE ATTACK VERB (ENGINE_NOTES §0.14). Installs a vfunc hook, so it is
    // OFF until measured -- #45: one new engine mechanism per release, proven
    // before it is trusted.
    inline std::atomic<bool>  g_commandTarget{ false };
    // DIK code for the focus hotkey. 0x2B is backslash -- unbound in vanilla
    // Skyrim, so it will not fight an existing control. 0 disables.
    inline std::atomic<int>   g_focusKey{ 0x2B };
    // OFF by default: it mutates player-visible equipment for a payoff that is
    // still unproven, and #57 says do not ship what you told yourself to probe.
    // The test guide turns it on for the session that measures it.
    inline std::atomic<bool>  g_equipToCast{ false };
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
    // hook, so OFF by default (#45).
    inline std::atomic<bool>  g_casterHook{ false };
    // 0 = LOG only (observe the AI's answer, change nothing -- the read-only
    // experiment). 1 = FORCE (override a veto to YES when latched).
    inline std::atomic<int>   g_casterMode{ 0 };

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
    inline std::atomic<float> g_suppressWindow{ 1.5f };
    // -- logistics (DESIGN §4.8) ---------------------------------------------
    // The whole non-combat table is GATED OFF by default, like every new
    // subsystem (#45): a follower who loots and drinks changes player-visible
    // inventory and world state, so it does not run until asked. With this off,
    // Scheduler evaluates only the combat table and MFO is byte-identical to
    // today out of combat.
    inline std::atomic<bool>  g_logistics{ false };
    // FIRST DIBS BY DELAY (#22h). A corpse/container is not eligible for
    // follower looting until it has been in the follower's consideration radius
    // this many seconds -- long enough that a player who wants the good sword
    // has walked over and taken it. (DESIGN §4.8.3 names this fLootDelaySeconds;
    // the key is fFirstDibsDelay per the M6 brief -- a NEW key, so no #37
    // rename hazard.)
    inline std::atomic<float> g_firstDibsDelay{ 25.0f };
    // THE WAIVER, collapsed but NEVER zeroed (#22h). Once the player takes from
    // a ref its delay drops to this -- not to 0, because QuickLoot IE (Nexus
    // 181813, in 4 of 5 lists here) takes items ONE AT A TIME over several
    // seconds, and the waiver timer RESETS on every take, so the follower moves
    // in this many seconds after the player's LAST take, not their first.
    inline std::atomic<float> g_quickLootWaiver{ 4.0f };

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
