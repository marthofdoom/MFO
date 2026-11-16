#pragma once
#include "PCH.h"
#include <memory>
#include <vector>

// iaus/EffectMirror.h -- THE WORKER'S ACTIVE-EFFECT READ (INVARIANTS #84, ENGINE_NOTES 0.30 amendment 2026-11-16).
//
// An actor's active-effect list (MagicTarget::GetActiveEffectList, a BSSimpleList<ActiveEffect*>) is changed by that actor's own
// update, by every spell hit and by every expiry, with no lock a worker read would respect. MFO's AddTask tick runs on a job worker
// (0.30), so it never walks one. It reads a MIRROR:
//   * built on the MAIN thread (the player-Update seat) once per worker tick, for every actor in the HIGH process list (the party,
//     their allies and every foe near enough to fight), the player, and any other actor asked for in the last ~15 s;
//   * plain values per effect (the source form, the base MGEF, the flags, duration / elapsed / magnitude, and for a summon or
//     reanimate effect whether its commanded actor is resolved and alive), plus the base MGEFs the ENGINE's own
//     MagicTarget::HasMagicEffect answers yes for (asked on main, so the worker's answer is the engine's, not a re-derivation);
//   * immutable once published. Get() never waits: an actor with no readable mirror returns null ("not yet") and is asked for.
// It replaces SpellBook's foe-only mirror (WantFoeFx / FoeFx now read this one) and the worker walks in cast/Summon.cpp,
// cast/Auto.cpp, logistics/EquipAuthority.cpp, logistics/Service.cpp, cast/SubstrateRoad.cpp and ProgProbe.cpp.
//
// Threads: Get / Want from any thread (a leaf mutex). BuildPass / BuildLive read the engine: MAIN ONLY, except BuildLive through
// Get where MainThread::LiveReadsAllowed() (the seat, the main thread outside a tick, or no pump): then the list is read in place.
// Freshness: one tick (133 ms) normally. A mirror older than kMaxAgeTicks (the main thread stalled / paused) is not readable.
namespace MFO::EffectMirror {

    struct Fx {
        const RE::MagicItem*     spell = nullptr;       // ActiveEffect::spell (the source form: identity compare and form reads only)
        const RE::EffectSetting* base = nullptr;        // ActiveEffect::effect->baseEffect (null when the effect item is missing)
        bool   inactive = false;                        // ActiveEffect::Flag::kInactive
        bool   dispelled = false;                       // ActiveEffect::Flag::kDispelled
        float  duration = 0.0f;                         // ActiveEffect::duration
        float  elapsed = 0.0f;                          // ActiveEffect::elapsedSeconds
        float  magnitude = 0.0f;                        // ActiveEffect::magnitude
        bool   summon = false;                          // skyrim_cast<SummonCreatureEffect*> succeeded
        bool   reanimate = false;                       // else skyrim_cast<ReanimateEffect*> succeeded
        bool   commandedResolved = false;               // its commandedActor handle resolves
        bool   commandedLive = false;                   // ... to an actor that is not dead and not deleted
        RE::FormID commandedId = 0;                     // ... whose FormID this is
        [[nodiscard]] bool Live() const noexcept { return !inactive && !dispelled; }
    };

    struct Mirror {
        RE::FormID      actor = 0;
        bool            hasList = false;                // the actor had a magic target and an effect list (false = unreadable there too)
        std::vector<Fx> fx;                             // the list's order
        std::vector<const RE::EffectSetting*> engineHas;// base MGEFs for which MagicTarget::HasMagicEffect returned true (sorted)
        [[nodiscard]] bool HasMagicEffect(const RE::EffectSetting* a_base) const;
    };
    using Ptr = std::shared_ptr<const Mirror>;

    // The effects on a_actor for THIS tick, or null = not yet (asked for). Never null for a non-null actor where
    // MainThread::LiveReadsAllowed().
    [[nodiscard]] Ptr Get(RE::Actor* a_actor);
    // By FormID (a reader that only holds an id). Same answer; the live path resolves the id.
    [[nodiscard]] Ptr Get(RE::FormID a_actor);
    // Keep a_actor mirrored (the party and the player each tick; the IAUS foes). No read.
    void Want(RE::FormID a_actor);
    // Would Get(a_actor) answer now? Asks like Get does (uncounted). The tick gate (Diagnostics.cpp).
    [[nodiscard]] bool Ready(RE::FormID a_actor);

    void BuildPass();                       // MAIN THREAD, once per worker tick (Diagnostics.cpp posts it with InvSnap::BuildPass)
    void Clear();                           // revert / load
    void LogStats();                        // `[fxmirror]`, one line per 60 s

    // MAIN THREAD (or LiveReadsAllowed): the walk itself.
    [[nodiscard]] Ptr BuildLive(RE::Actor* a_actor);

}
