#pragma once
#include "PCH.h"

// Cooldowns -- the ONE runtime "when did this last fire" store for pacing (class-plan card G9; card I2's IAUS
// `cooldown` consideration is meant to share it, so "10 s power-attack cooldown" means the same thing in both).
//
// WHAT IT IS: a map (actor FormID, key kind, key id) -> the steady_clock instant that thing last FIRED. The
// caller owns the duration (a gambit row's own Gambit::cooldownTenths for G9) and asks Remaining(...) with it,
// so editing a row's cooldown on the board takes effect at once, even mid-cooldown.
//
// RUNTIME-ONLY, NEVER SERIALIZED: a cooldown that resets on load / revert is the documented contract (card G9).
// Cleared by ClearAll from ResetAllState AFTER Diagnostics::StopPump. Only the configured seconds are saved
// (FLWR v6, the gambit row; Serialization.h).
//
// NOT EVALUATOR STATE (#22): Eval::Evaluate never reads this. The combat scan (Scheduler.cpp) consults it AFTER a
// rule matched, as an actuation-side pacing gate that turns the rule into a TRANSPARENT outcome ("cooling down
// (Ns)"), the same shape as the drink / cast cooldowns that already live in actuation. Losing the store only
// lets a rule fire early; it never changes which rule the evaluator chose.
//
// CLOCK: steady_clock (real time). The pump does not tick while the game is paused, but real time still passes,
// so a cooldown can expire inside a menu. Acceptable for a 10 s pacing gate; noted for IAUS.
//
// THREADING: g_mx guards the map; no engine call is ever made under it. Callers today: the Scheduler's combat
// scan (AddTask job worker) and ResetAllState (main, pump drained).
namespace MFO::Cooldowns {

    // What a key's id means. APPEND ONLY (IAUS adds its own kinds, e.g. an (action, form) pair packed in id).
    enum class Kind : std::uint8_t {
        Rule = 0,   // id = the gambit row's runtime uid (State.h Gambit::uid, NextRuleUID; never 0)
        IausAction = 1,   // id = Iaus::CooldownId(key): (family, posture, form) of an IAUS candidate (card I2)
        Follower = 2,     // id = a kFollower* slot: ONE cooldown per follower shared by every gambit row AND the IAUS
                          // candidate of that action (marth Q69 taunt, Q73 power attack)
    };
    // Kind::Follower ids. Mirrored by Iaus::SharedCooldownId (iaus/Core.h, host-pure); Engine.cpp static_asserts they agree.
    inline constexpr std::uint64_t kFollowerPowerAttack = 1;
    inline constexpr std::uint64_t kFollowerTaunt = 2;

    // Record that (a_actor, a_kind, a_id) fired at a_now.
    void NoteFired(RE::FormID a_actor, Kind a_kind, std::uint64_t a_id,
                   std::chrono::steady_clock::time_point a_now);

    // Seconds left of an a_cooldownSec cooldown for (a_actor, a_kind, a_id) at a_now. 0 = ready (never fired,
    // or the cooldown has run out, or a_cooldownSec <= 0).
    [[nodiscard]] float Remaining(RE::FormID a_actor, Kind a_kind, std::uint64_t a_id, float a_cooldownSec,
                                  std::chrono::steady_clock::time_point a_now);

    // Every entry, every actor. ResetAllState (after StopPump).
    void ClearAll();

    // ── G9: the gambit-row view ───────────────────────────────────────────────────────────────────────
    // "cooling down (12s)": the transparent-skip reason the combat scan writes to the chain and to the board's
    // lastFailReason (the G10 skip path). Whole seconds, rounded UP, never "0s" while still cooling.
    [[nodiscard]] std::string CoolingReason(float a_remainingSec);
    inline constexpr std::string_view kCoolingPrefix = "cooling down (";

}
