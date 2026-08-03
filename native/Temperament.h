#pragma once
#include "PCH.h"

// THE TEMPERAMENT SEED (GAMBIT_FLAIR #1). One stable scalar per follower so
// each one is *consistently* themselves -- Lydia always the deliberate one,
// Erik always the eager one -- without a personality system.
//
// A PURE FUNCTION of the FormID: no state, no serialization, no RNG. The same
// follower gets the same value every session, every save, every machine.
// Consumers scale timings by it (suppression-window tempo, combat-entry ready
// beat, cast-grace offset); the MCM knob always stays the CENTER of the band
// and the deviation is small and deterministic, so legibility survives --
// the [eval] log timestamps still explain every cadence.
//
// Knuth multiplicative hash: multiply by 2654435761 (2^32/phi), take ten bits
// from the middle. Cheap, well-mixed for sequential FormIDs (follower plugins
// cluster their NPC records), and header-only so Scheduler, Actuation and
// Confidence can all see it without a new translation unit.

namespace MFO {

    [[nodiscard]] inline float Temperament(RE::FormID a_id) {
        const std::uint32_t mixed = (a_id * 2654435761u) >> 16;
        return static_cast<float>(mixed & 0x3FFu) / 1023.0f;
    }

}
