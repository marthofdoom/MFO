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
