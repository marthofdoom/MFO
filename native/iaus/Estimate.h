#pragma once
#include <algorithm>

// IAUS estimates: pure formulas the engine side ROLLS with and the IAUS facts READ, so the two can never drift.
// Std types only (host-compilable, no CommonLib): the harness includes it directly.
namespace MFO::Iaus::Estimate {

    // The chance a taunt lands: p = clamp(0.5 + 0.05 x (taunter level - foe level), 0.1, 0.9). The roll in
    // Taunt.cpp (Resolve) calls this; marth Q72 keeps the level rule.
    [[nodiscard]] inline float TauntChance(int taunterLevel, int foeLevel) {
        return (std::clamp)(0.5f + 0.05f * static_cast<float>(taunterLevel - foeLevel), 0.1f, 0.9f);
    }

}
