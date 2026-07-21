#pragma once
#include "PCH.h"

namespace MFO {

    // Co-save identity. 'MFO0' / 'FLWR' -- both frozen once a build ships.
    inline constexpr std::uint32_t kSerID       = 'MFO0';
    inline constexpr std::uint32_t kRecFollowers = 'FLWR';

    // INVARIANTS.md #12: bump on every schema change; keep a reader for EVERY
    // shipped version FOREVER; fields are append-only.
    //   v1 - P0: follower map {rapport, rank, gambits[], tutored[], overrides[]}
    inline constexpr std::uint32_t kSchemaVersion = 1;

    void SaveCallback(SKSE::SerializationInterface* a_intfc);
    void LoadCallback(SKSE::SerializationInterface* a_intfc);
    void RevertCallback(SKSE::SerializationInterface* a_intfc);

}
