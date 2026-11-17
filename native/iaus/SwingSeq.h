#pragma once
#include "PCH.h"

#include <cstdint>

// iaus/SwingSeq.h -- THE ENGINE SIDE OF THE SEQUENCE TRACKING (marth 2026-11-17: "it will be the sequence thats tracked").
// Record: Docs/research/2026-11/anim-reach.md. The learner keyed by it: iaus/ReachBook.h.
//
// Lists with combo frameworks (MCO / ADXP style: Tuxborn, LoreRim) chain light attacks (and maybe power attacks) into a
// sequence he cannot steer once it starts. The swing events of that chain are raised by the animation graph, so this module
// listens to the follower's graph and COUNTS, per follower: swings (weaponSwing / weaponLeftSwing, a both-hands swing within
// kSameSwingMs counts once), power-window and bash tag counts (OBSERVABILITY ONLY: they do not key the learner, review F1; the swing log line prints them per chain), and every event
// it saw at all (the "is the sink really attached" proof). LIST-AGNOSTIC: tag names only, no list file, no framework check.
// A graph sink sees graph-RAISED events only (never NotifyAnimationGraph inputs): attackStart is invisible, the swing is not.
//
// THREADING (the [atk-obs] contract, Diagnostics.cpp): the sink runs on whatever thread the graph dispatches on, so it
// touches fixed-size atomics ONLY (no map, no lock, no allocation, no log, no engine call but the holder's FormID). The
// attach / detach are MainThread::Post'd (graph mutation is main-thread work), capturing the FormID + slot only.
// Service / Read / Clear are the g_followers WORKER's (the learner's domain, #4).
namespace MFO::Iaus::SwingSeq {

    struct Obs {
        bool          live = false;          // the sink is attached for him and has seen at least one graph event
        std::uint32_t swings = 0;            // monotonic
        std::uint32_t power = 0;             // monotonic
        std::uint32_t bash = 0;              // monotonic
        std::int64_t  lastSwingMs = 0;       // steady-clock ms of the last counted swing, 0 = none
    };

    // WORKER: claim / refresh his slot and (every kAttachMs) post the idempotent attach. Cheap when nothing is due.
    void Service(RE::Actor* a_actor);
    // WORKER (any thread, atomics only): his counters. live=false when he has no slot or no event has arrived.
    [[nodiscard]] Obs Read(RE::FormID a_fid);
    [[nodiscard]] std::int64_t NowMs();
    // ResetAllState after StopPump (through Fills::Clear): zero every counter and free every slot (the sinks stay registered
    // on live graphs, and a freed slot fails their membership test).
    void Clear();

}
