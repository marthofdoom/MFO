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
    inline std::atomic<bool>  g_showHud{ true };

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
    // Seconds a two-handed wielder must go between weapon swaps. The off-hand
    // swap is free and ungated; stowing a greatsword is not.
    inline std::atomic<float> g_twoHandedDebounce{ 6.0f };

    // -- evaluator (M5) ------------------------------------------------------
    inline std::atomic<bool>  g_seedEvaluatorRules{ false };
    inline std::atomic<bool>  g_profileEvaluator{ false };
    inline std::atomic<float> g_suppressWindow{ 1.5f };
    // Which caster a gambit spell goes through. PROVEN 2026-07-21: NO source
    // animates -- kLeftHand/kRightHand/kOther were all tried in the field and
    // behave like kInstant (ENGINE_NOTES §0.10). Kept configurable because it
    // costs nothing and the next engine question may need it; defaulted back to
    // 3 (kInstant), which at least names what actually happens.
    inline std::atomic<int>   g_castSource{ 3 };


    // Reset-then-parse both files, seed then MCM. Safe to call repeatedly.
    // CURRENTLY CALLED ONLY AT kDataLoaded -- there is no MenuOpenCloseEvent
    // sink yet, so MCM edits need a restart until M7 adds one. ARCHITECTURE
    // 6 lists that sink as planned, not shipped.
    void Read();

}
