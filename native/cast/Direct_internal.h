#pragma once
// cast/Direct_internal.h -- the DIRECT-DELIVERY road's shared state, for the five
// cast/ TUs cut from the old cast/Direct.cpp (Direct, DirectSelf, DirectTarget,
// CanAct, HealObs) and NOTHING else. Split out by the Direct.cpp split
// (2026-09-29, REVIEW-BACKLOG MFO-B152): a pure move, proven function by function
// with tools/splitcheck. Each symbol below was in an anonymous namespace of
// Direct.cpp and is now used from a second TU of the road, so it gained external
// linkage; bodies are unchanged and stay where they were.
//
// WHY A SEPARATE HEADER, not cast/Actuation_internal.h: cast/CastOn.cpp includes
// Actuation_internal.h and keeps its own FILE-LOCAL twin of LogApmfRefusal (and of
// its throttle state). Declaring this road's LogApmfRefusal there would make every
// CastOn.cpp call ambiguous. Only the direct road's TUs include this file.
#include "Actuation_internal.h"

namespace MFO::Actuation {

    // ── FORCED SELF-CAST: the UNIVERSAL direct trigger (SPEC-self-cast-forced) ──
    // The MFO_CastPackageSelf alias route EQUIPS the spell but never TRIGGERS the
    // cast, and is declined outright on package-locked custom followers (Lucien).
    // So self-cast bypasses packages and applies the effect DIRECTLY -- follower-
    // agnostic, effect + magicka only, NO equip and NO channel:
    //
    //   * NEVER EQUIP THE SPELL. CastSpellImmediate (kInstant) applies the effect
    //     without the spell in hand. Leaving a light spell (Candlelight/Magelight)
    //     equipped let the follower's OWN AI spam-cast it -- 55+ non-MFO lights
    //     piled up to a ShadowSceneNode light-limit CTD (deck 2026-08-19). The
    //     cast animation is DEFERRED (polish pass), so the equip/HoldStow/caster-
    //     drive scaffolding is gone entirely.
    //   * FIRE (CastSelfDirect, every service/combat tick the rule wins): register
    //     the entry, refresh lastFired, and -- once per beat (kConcApplyPeriod ~1 s
    //     for a CONCENTRATION spell, per-second authored magnitude/cost; once per
    //     fCastCooldown for FF) -- apply the
    //     effect + spend magicka (§5.3) via ApplySelfEffect. The ALREADY-ACTIVE
    //     guard there (the foe cast's own HasMagicEffect check) blocks re-applying
    //     a duration self-buff/light while it is still up, so exactly ONE light
    //     per effect-duration cycle. An instant heal re-fires when HP drops again.
    //   * RELEASE (SelfCastReconcile, each tick): when the rule stops re-firing
    //     (goes stale, or the follower unloads) DISPEL a lingering ward/effect so
    //     it cannot persist as a stuck gameplay buff. NO time cap -- a long buff
    //     lives its authored duration while the rule wins. Covers rule-disabled /
    //     condition-false (the entry simply goes stale). Nothing to unequip.
        struct SelfCastState {
            RE::FormID            spell = 0;
            SelfClock::time_point started{};
            SelfClock::time_point lastFired{};   // last time the rule re-fired (release clock)
            SelfClock::time_point lastApply{};   // last effect/magicka application (apply pacing)
            float                 cap   = 0.0f;  // per-stream randomized time cap (DrawConcCap)
            // CHARGE FOR TIME (v2.0.12): a TIMED stream (momentary concentration --
            // see StreamCharge) is billed for the seconds its effect actually ran.
            // paidThrough = the instant the caster has paid up to (epoch = nothing
            // paid yet); window = the sustain window the beats pin, i.e. how long
            // the effect keeps running past the last beat. Worker-serial.
            SelfClock::time_point paidThrough{};
            float                 window = 0.0f;
            bool                  timed  = false;
        };
        extern std::unordered_map<RE::FormID, SelfCastState> g_selfCast;   // cast/Direct.cpp

        // The APMF-refusal line (cast/Direct.cpp; its throttle state stays file-local there).
        void LogApmfRefusal(RE::FormID a_follower, const char* a_what, RE::FormID a_spell,
                            RE::FormID a_target, const char* a_hand);

        // The apply substrate (cast/Direct.cpp) the self and target streams call.
        bool ConcMomentary(RE::SpellItem* a_sp);
        float BeatChargeSec(bool a_timed, SelfClock::time_point& a_paidThrough,
                            SelfClock::time_point a_prevApply, float a_window,
                            SelfClock::time_point a_now);
        float SettleSec(bool a_timed, SelfClock::time_point a_paidThrough,
                        SelfClock::time_point a_lastApply, float a_window, SelfClock::time_point a_now);
        void PostSettle(RE::FormID a_casterID, RE::FormID a_spellID, float a_sec,
                        const char* a_road, const char* a_reason);
        void ApplySelfEffect(RE::FormID a_id, RE::FormID a_spellID, float a_chargeSec);
        void SelfCastEndActor(RE::FormID a_id, RE::FormID a_spellID);
        void ApplyTargetEffect(RE::FormID a_casterID, RE::FormID a_targetID,
                               RE::FormID a_spellID, bool a_guard, float a_chargeSec);
        void TargetCastEndActor(RE::FormID a_targetID, RE::FormID a_spellID, RE::FormID a_ownerID);

        // The revert/load resets ClearSelfCasts (cast/Direct.cpp) calls.
        void ResetCanActState();   // fix/mfo-can-act -- defined with its state in cast/CanAct.cpp
        void ResetHealObs();       // feat/mfo-animheal-p0 -- defined with its state in cast/HealObs.cpp

}
