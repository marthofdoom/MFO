#pragma once
#include "PCH.h"

#include <cstdint>

// native/probes/ -- N0, THE PASSIVE FIELD PROBES FOR THE MOVEMENT AI (Docs/research/2026-11/iaus-sim-v4.md 10.2 row N0, sections 8.3 / 8.7 /
// 8.8 / 8.9). Every number the movement design leans on (what a traversal costs, whether a follower takes a navmesh drop link, what a
// engine ray costs, how the ch.24 point road behaves as a general mover, what a "weird" place looks like in the log) is MEASURED by these
// lines in a play session before any later native slice trusts it. tools/field/calib_traversal.py reads them.
//
// STRICTLY PASSIVE: no engine write, no hook seat, no new engine id or address, no new thread, no hotkey, no co-save, no ABI. One INI
// section [Probes] (Config.h g_probe*), default ON in a dev (MFO_DEV_SELFTEST) DLL only, every probe rate limited (iProbeLinesPerMin).
// THREADING: Tick / NoteLived / PointOpen / PointClose run on the worker (the serial pump, the g_followers domain, #4). NoteRay and the
// RayTimer run on the MAIN thread inside Sightline and touch atomics only. The graph sink runs on whatever thread the engine dispatches
// on and touches fixed-size atomics only (the SwingSeq / [atk-obs] contract). Nothing here is posted across a save: a posted lambda
// captures a FormID and re-resolves it.
//   Tick()                 WORKER, once per pump tick (Diagnostics.cpp, after AtkObserveTick): sample each managed follower, run the
//                          traversal drain, the drop / jump watch, the point watch and the once-a-minute summaries.
//   NoteLived()            WORKER: one lived trust sign (8.9) from an end the project already logs (a blocked leg, an engine drop).
//   RayTimer               MAIN: a scope that times ONE engine ray Sightline already casts. Counts only (no log on the main thread).
//   PointOpen / PointClose WORKER: the ch.24 POINT claims MFO already files (give ground, disengage), watched for what he does inside R.
//   Clear()                ResetAllState (after StopPump): drop every table. The sinks stay registered on live graphs and fail their
//                          slot membership test once the slot is freed.
// To add a probe: a new file here, a Config key in [Probes], a row in the INI reference, a line in the calibration script.
// To add a traversal mod: a row in kEvents / kVars (Traversal.cpp). No code path names a mod.
namespace MFO::Probes {

    // The lived trust signs of 8.9 that need no new engine read. Rows of kLivedNames (Probes.cpp).
    enum class Lived : std::uint8_t {
        EngineDropped,      // the engine refused a path we found (a ch.24 end EngineDropped)
        LegBlockedStatic,   // a ch.19 loot leg ended BLOCKED by a static (no actor in the lane)
        LegStuckTimeout,    // a ch.19 loot leg ended STUCK-TIMEOUT: the plan said walkable, he made no ground
        Jump,               // a position jump between two samples larger than his speed allows (a teleport, a warp, a script)
        Falling,            // a height loss with no graph event under it
        TraversalUnplanned, // a traversal move while a loot leg was in flight (the leg's route had none)
        kCount
    };

    // The engine rays Sightline already casts, timed where they are cast.
    enum class RaySite : std::uint8_t {
        OwnRay,        // CustomRay's bhkWorld PickObject (up to 3 per call)
        PointRay,      // PointSeenNow's bhkWorld PickObject (up to 6 per call)
        EngineLos,     // Actor::HasLineOfSight (the engine's cached combat-AI answer)
        kCount
    };

    void Tick();
    void Clear();
    void NoteLived(Lived a_sign, RE::FormID a_actor, const char* a_detail);
    void PointOpen(RE::FormID a_actor, const char* a_kind, const RE::NiPoint3& a_point, float a_radius);
    void PointClose(RE::FormID a_actor, const char* a_why);

    void NoteRay(RaySite a_site, std::int64_t a_ns);

    // MAIN thread. Cheap when bProbeRayCost is off (one relaxed load). Wrap exactly one engine ray.
    class RayTimer {
    public:
        explicit RayTimer(RaySite a_site) noexcept;
        ~RayTimer();
        RayTimer(const RayTimer&) = delete;
        RayTimer& operator=(const RayTimer&) = delete;
    private:
        RaySite       m_site;
        std::int64_t  m_t0;   // steady-clock ns, 0 = not timing
    };

}
