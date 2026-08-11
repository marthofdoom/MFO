#pragma once
#include "PCH.h"

namespace MFO {

    // Co-save identity. 'MFO0' / 'FLWR' -- both frozen once a build ships.
    inline constexpr std::uint32_t kSerID       = 'MFO0';
    inline constexpr std::uint32_t kRecFollowers = 'FLWR';

    // #69: a follower's OWN gear (Logistics::g_stockGear), SEPARATE from FLWR
    // -- its own record type and its own version, so it can evolve (or fail to
    // parse on a downgrade) without touching the FLWR schema at all.
    inline constexpr std::uint32_t kRecStock    = 'MSTK';
    inline constexpr std::uint32_t kStockVersion = 1;

    // INVARIANTS.md #12: bump on every schema change; keep a reader for EVERY
    // shipped version FOREVER.
    //
    //   v1 - shipped in v0.1.0-v0.3.0: {rapport, rank, tables[], TUTORED[],
    //        overrides[]}
    //   v2 - tutoring removed (DESIGN.md 5.4): {rapport, rank, tables[],
    //        overrides[]}
    //   v3 - #68: cast-target resolution ladder. Each gambit gains
    //        subjectActorForm (a FormID, 0 = use subjectSelector's enum
    //        instead), written right after subjectSelector. Read ONLY when
    //        version >= 3; older records simply have none (defaults to 0).
    //   v4 - #65: FollowerState gains combatClassOverride (a per-follower
    //        forced combat stance: 0=Auto, 1=Melee, 2=Ranged, 3=Cast),
    //        written right after st.rank. Read ONLY when version >= 4; older
    //        records simply have none (defaults to 0 -- Auto, no override).
    //
    // The v1 block was briefly deleted WITHOUT a bump, on the reasoning that no
    // save had ever held an MFO record. True at the time; it stopped being true
    // the moment saving with the mod active was planned. Bumped to v2 with a v1
    // reader that consumes and discards the tutored block -- because "no save
    // has it yet" is a fact with an expiry date, and a version number is not.
    inline constexpr std::uint32_t kSchemaVersion = 4;

    void SaveCallback(SKSE::SerializationInterface* a_intfc);
    void LoadCallback(SKSE::SerializationInterface* a_intfc);
    void RevertCallback(SKSE::SerializationInterface* a_intfc);

    // True when the last load saw a co-save record NEWER than this DLL can
    // read. INVARIANTS #12 requires an ON-SCREEN warning, not just a log line:
    // the user who downgrades and saves never reads the log until the data is
    // already gone. Surfaced at kPostLoadGame.
    bool ConsumeNewerSaveWarning();

}
