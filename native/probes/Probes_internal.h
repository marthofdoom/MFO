#pragma once
#include "Probes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// native/probes/ shared state. The only header another probes file includes. Public surface: Probes.h.
namespace MFO::Probes {

    // EVERY TUNABLE OF THE PROBES IN ONE NAMED TABLE (design for future tweaks). Placeholders until the first field session shows the
    // real distributions; the lines print the raw numbers, so a threshold here only decides what gets a line, never what a number is.
    struct Tuning {
        std::int64_t sampleMs        = 120;     // a follower is sampled at most this often (the pump runs every ~133 ms)
        float        dropMinZ        = 150.0f;   // height lost (units) before a drop / fall is reported
        float        fallMinSpeed    = 450.0f;   // a FALL needs this per-sample fall speed (u/s) at some point of the descent (a run down a stair is ~260-350)
        float        steepSlope      = 1.7f;     // a STEEP descent (no fall speed, but height >= dropMinZ) is still logged when height / across is at least this
        std::int64_t dropWindowMs    = 2500;     // the peak height must lie this recent
        float        landedDz        = 4.0f;     // |dz| between two samples under this = landed
        std::int64_t dropEventSlackMs = 1500;    // a graph event this close before the peak (or after) belongs to the drop
        float        jumpMinDist     = 600.0f;   // horizontal units between two samples ...
        float        jumpMinSpeed    = 1800.0f;  // ... and units per second, both over: a jump, not a sprint (a horse is ~1100)
        std::int64_t jumpMaxDtMs     = 1500;     // samples further apart than this are not compared
        std::int64_t livedPerActorMs = 5000;     // one [probe-trust] line per (sign, actor) per this
        std::int64_t moveWatchdogMs  = 6000;     // a traversal start with no stop this long is reported NO-STOP (the mod's own watchdog is 4 s)
        std::int64_t attachMs        = 2000;     // re-post the (deduped) sink attach this often: a graph is rebuilt on a 3D reload
        std::int64_t idleFreeMs      = 60000;    // a sink slot not serviced this long is freed (dismissed follower)
        std::int64_t pointLineMs     = 2000;     // and prints a progress line this often while it stands
        std::int64_t raySummaryMs    = 30000;    // [probe-ray] summary period
        std::int64_t minuteMs        = 60000;    // the per-probe line budget window
    };
    inline constexpr Tuning kTuning{};

    inline constexpr std::size_t kRing = 32;     // samples kept per follower (~4 s at 120 ms)

    struct Sample {
        std::int64_t  ms = 0;
        float         x = 0, y = 0, z = 0;
        std::uint32_t cell = 0;
        std::uint32_t space = 0;                  // interior cell FormID, else worldspace FormID: the ring resets on a change of THIS, not of the 4096-unit exterior cell
    };

    // Worker-only per-follower track (Probes.cpp owns the table).
    struct Track {
        std::array<Sample, kRing> ring{};
        std::size_t  n = 0;                       // samples ever pushed; the newest is ring[(n - 1) % kRing]
        std::int64_t lastEventMs = 0;             // the last traversal graph event seen for him (Traversal.cpp)
        bool         falling = false;
        float        maxVz = 0;                   // the largest per-sample fall speed (u/s) since the peak
        Sample       peak{};
        std::string  name;
    };

    enum class Kind : std::uint8_t { Trav, Drop, Ray, Trust, Point, kCount };

    std::int64_t NowMs();
    // The per-probe, per-minute line budget (iProbeLinesPerMin). False = the line is suppressed and counted; a suppressed count is said once a minute.
    bool LineAllowed(Kind a_kind);
    bool Enabled(Kind a_kind);

    const Sample* Newest(const Track& a_t);
    // The newest sample at or before a_ms; nullptr when none is (the track is empty or every sample is AFTER the event).
    const Sample* AtOrBefore(const Track& a_t, std::int64_t a_ms);
    Track* TrackOf(RE::FormID a_fid);
    // The "what MFO had in flight for him" word: leg, offense, heal, range, disengage, point, cast, pin, sneak, leash, or "none". WORKER.
    std::string InFlight(RE::Actor* a_actor);
    std::string PosWord(float a_x, float a_y, float a_z);
    // Lived signs logged since a_fid's counter was read (Traversal.cpp's "did a reader of 8.8 (3) fire during the move").
    std::uint32_t LivedCount(RE::FormID a_fid);

    // Traversal.cpp
    void TraversalService(RE::Actor* a_actor);                 // WORKER: slot + attach, per follower per tick
    void TraversalDrain(std::int64_t a_nowMs);                 // WORKER: pair and log the events the sink collected; the no-stop watchdog
    void TraversalClear();
    // PointWatch.cpp
    bool PointWatching(RE::FormID a_fid);
    void PointSample(RE::Actor* a_actor, const Sample& a_cur);
    void PointClear();
    // RayCost.cpp
    void RayTick(std::int64_t a_nowMs);
    void RayClear();

}
