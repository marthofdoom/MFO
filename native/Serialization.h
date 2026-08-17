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

    // Follower progression (component 2 of the ESL addon): per-follower
    // enrollment/class/level/perk+skill allocations (ProgAllocator::g_prog).
    // A THIRD independent record -- own type, own version, never touches
    // FLWR/MSTK. Layout + ingestion rules live in ProgAllocator.cpp
    // (CoSaveSave/CoSaveLoad); design doc §8.
    //
    //   v1 - header {lastPlayerLevel u16}, then per follower {formID, flags,
    //        class(u8 ordinal), progressionLevel, sharedGrowthRemainder,
    //        unspentPerk f32, perkAlloc[]{formID,rank},
    //        skillAlloc[]{av,points,lastWrittenBase}, enrollBaseline[]{av,f32}}
    //   v2 - §16 manual skill points: flags bit 0x10, manualBaselineLevel u16,
    //        manualPointsApplied u16, manualExcludedLevels u16,
    //        nativeTreePerksAtEnroll u16 (all gated version>=2); skillAlloc
    //        gains manualPoints f32.
    //   v3 - §18.6 Stage 2 N-declared classes: the class field WIDENS from a
    //        1-byte ordinal to a 4-byte RE::FormID (the class-def FLST id,
    //        ResolveFormID-stable) at the SAME position. v<3 readers consume
    //        the 1 byte and MIGRATE ordinal k → k-th declared class; v>=3
    //        reads the FormID and ResolveFormID's it.
    inline constexpr std::uint32_t kRecProgression = 'PRGN';
    inline constexpr std::uint32_t kProgVersion    = 3;   // v3: §18.6 N-declared class FormID

    // #76 force-hold: a FOURTH independent record — the weapons MFO force-equipped
    // (prevent-removal) for an active equip gambit. The engine's forceEquip lock
    // serializes into the .ess, but g_forcedWeapon does not, so without this a
    // save made mid-hold reloads with a latent lock and no record to release it
    // (the follower stuck holding the weapon, unable to cast). Persisting it lets
    // the load path clear every stale lock (Actuation::CoLoad releases them).
    inline constexpr std::uint32_t kRecForcedWeapon  = 'FWPN';
    inline constexpr std::uint32_t kForcedWeaponVersion = 1;

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
