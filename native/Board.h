#pragma once
#include "PCH.h"

// The Field Kit — the in-game overlay.
//
// Pulled forward from M7 (marth): reading a log after the fact is a hopeless
// feedback loop for a behaviour mod. This is the READ-ONLY half of the board:
// it shows live state so every milestone from here is observable while it
// happens. Rule editing comes later; the plumbing here is what M7 needs anyway.
//
// Architecture is MEO's, transcribed rather than re-derived (ENGINE_NOTES §9):
// three trampoline hooks installed before the renderer exists, a render thread
// that draws from a mutex-guarded snapshot, and an input hook that swallows
// input while open.

namespace MFO::Board {

    // One row per tracked follower. PLAIN VALUES ONLY -- no engine pointers
    // ever cross to the render thread (ARCHITECTURE.md §3.1).
    // One rule as the board sees it -- a flat copy, never a pointer into the
    // live table (the board draws on the render thread; the tables are
    // main-thread-only).
    struct RuleView {
        std::uint32_t uid = 0;
        std::string condOp, actOp;
        float       param = 0.0f;
        RE::FormID  spell = 0;
        std::string spellName;
        // #68: the cast-target resolution subject, mirrored from the Gambit
        // for display. subjectName is resolved MAIN-THREAD-ONLY in
        // FillRuleViews (same rule as spellName just above) -- the render
        // thread only ever reads the plain string, never an engine pointer.
        std::uint8_t subject = 0;
        RE::FormID   subjectActorForm = 0;
        std::string  subjectName;
        bool        enabled = true;
        bool        lastFired = false;
        std::string fail;
        // When the rule last fired (flair #9). A plain steady_clock stamp --
        // safe to age from the render thread; epoch default = no pulse.
        std::chrono::steady_clock::time_point firedAt{};
    };

    struct FollowerRow {
        RE::FormID    id = 0;
        std::string   name;
        std::uint32_t rapport = 0;
        std::uint8_t  rank = 1;
        std::uint8_t  combatSlots = 0;
        std::uint8_t  logisticsSlots = 0;
        std::uint8_t  combatRules = 0;
        std::uint8_t  logisticsRules = 0;
        bool          active = false;
        bool          teammate = false;
        bool          commanded = false;
        bool          inCombat = false;
        float         healthPct = 1.0f;
        float         magickaPct = 1.0f;
        float         staminaPct = 1.0f;
        float         distance = 0.0f;
        std::vector<RuleView> combat, logistics;
        std::vector<std::pair<RE::FormID, std::string>> knownSpells;
        // #4: spells the PLAYER carries a spellbook for that this follower does
        // NOT yet know -- offered in the picker as "Name (spellbook)", teachable
        // (consumes the book) on confirm.
        struct Teachable { RE::FormID spell = 0; RE::FormID book = 0; std::string name; };
        std::vector<Teachable> teachableSpells;
        // #68: the cast-target picker's option list -- the PLAYER's live
        // name and every OTHER active follower by name (never this row's
        // own actor). Same "resolved main-thread-only, plain values cross to
        // the render thread" rule as knownSpells above.
        std::string playerName;
        std::vector<std::pair<RE::FormID, std::string>> alliesForPicker;
    };

    struct Snapshot {
        std::vector<FollowerRow> rows;
        // headline numbers, mirroring the log dump
        std::uint32_t combatEvents = 0;
        double        minutes = 0.0;
        std::uint32_t kills = 0;
        std::uint32_t rapport = 0;
        // config in force
        float rate = 1.0f, killVal = 1.0f, bossMult = 5.0f, dragonMult = 10.0f, radius = 3000.0f;
        int   rank2 = 0, rank3 = 0, rank4 = 0, rank5 = 0;
        bool  allowSummons = false;
        int   quirksActive = 0, quirksInactive = 0;
        std::uint64_t frame = 0;
        std::uint32_t evalTicks = 0;
        double        evalMs = 0.0;
        // last credited kill, for explaining a boss/standard classification
        std::string lastKillName, lastKillKind;
        std::uint16_t lastVictimLevel = 0, lastPlayerLevel = 0;
        float lastAwarded = 0.0f;
        int   lastCredited = 0;
        bool  lastValid = false;
        int   bossLevelDelta = 5;
    };

    // Install the three trampoline hooks. MUST be called from SKSEPluginLoad,
    // before the renderer initializes.
    void Install();

    // Rebuild the snapshot from live state. MAIN THREAD ONLY.
    void PublishSnapshot();

    // Full panel. Takes input while open, so it cannot be watched during
    // combat -- that is what the HUD is for.
    void Toggle();
    bool IsOpen();
    // Drop queued edits on revert -- a command from the old save must never
    // apply to a freshly loaded one for the same FormID.
    void ClearPendingEdits();

    // Compact passive readout. Draws every frame, takes NO input, so it stays
    // legible while fighting. On a single-monitor setup this is the primary
    // observation surface, not a nicety.
    void ToggleHud();
    void SetHud(bool a_on);

    // True once D3D init succeeded. When false the overlay is unavailable and
    // the power falls back to the log dump -- the evaluator must never depend
    // on the renderer (INVARIANTS #25).
    bool IsAvailable();

}
