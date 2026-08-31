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

    // GENERAL per-follower FOLLOWER-ALLOCATION-STATE slot (host machinery, v1.1).
    // A THIRD independent record -- own type, own version, never touches
    // FLWR/MSTK. Layout + ingestion rules live in ProgAllocator.cpp
    // (CoSaveSave/CoSaveLoad); design doc §6/§8.
    //
    // v1.1 REFRAME (Phase 8, ZERO on-disk change): every field is GENERAL
    // allocation-engine state that ANY allocation add-on would carry -- an
    // enrolled flag, an OPAQUE plugin-qualified class-def reference {clsPlugin,
    // clsLocal} the host re-resolves but never INTERPRETS, allocated perks/skills
    // (FormID+value), HMS pools, battle counters, and the engine-detected fixed-
    // stat flag. It embeds NO add-on JUDGMENT: the ratios/verdicts/layout that
    // MEAN anything live in the manifest FORM the class reference points to, not
    // here. So deleting the add-on's manifest strands no judgment in this slot --
    // no add-on enrolls, g_prog stays empty, and this record writes only the
    // header + count=0 (Phase 9 acceptance test). The 'PRGN' fourCC VALUE +
    // kProgVersion are FROZEN for compat (the deployed Tuxborn v6 save) -- the
    // "Progression"/"Prog" naming is HISTORICAL (the sole add-on populating the
    // slot today is MFO_Progression); it is the general host slot, not an
    // add-on-specific schema.
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
    //        the 1 byte and MIGRATE ordinal k → k-th declared class; v==3
    //        reads the FormID and ResolveFormID's it.
    //   v4 - SEV-2 class-wipe fix (2026-08-17): the class field WIDENS AGAIN,
    //        from the bare 4-byte runtime FormID to a stable plugin-qualified
    //        identity {u16 pluginLen, plugin bytes, u32 localFormID}, at the
    //        SAME position. A session run WITHOUT the addon ESL could no longer
    //        ResolveFormID the v3 id → cleared it → the next save persisted 0 →
    //        class lost forever when the ESL returned. The plugin+local pair
    //        survives an addon-absent session (echoed back verbatim on save) and
    //        re-resolves when the ESL comes back. v==3 readers still consume
    //        exactly the 4 bytes; the v3 reader is KEPT (INVARIANT #12) — a v3
    //        record that resolves self-heals to v4 on its next save.
    //   v5 - §HMS class-redistribution: an HMS block is APPENDED at the very END
    //        of each per-follower record (after the baseline[] block), read ONLY
    //        when version>=5. Fixed order, no count prefix — per pool in fixed
    //        {Health, Magicka, Stamina} order: hmsBaseline(f32), hmsTarget(f32),
    //        hmsSkew(f32), hmsCumulative(f32); then battlesSinceLevelUp(u32),
    //        battlesOffClass(u32), offClassPool(u8), hmsCaptured(u8). NOTHING
    //        before this block moved or resized, so a v1–v4 save read by this v5
    //        DLL is byte-identical to before, and a v4 DLL reading a v5 save
    //        skips the whole record via the version>kProgVersion dispatch. All
    //        v1–v4 readers are KEPT untouched (INVARIANT #12). A v4 record read
    //        by this v5 DLL has no HMS block → hmsCaptured stays false → the
    //        first RecomputeHMS adopts the live base H/M/S as the baseline.
    //   v6 - §HMS fixed-stat grant (v1.1 Phase 3). THREE changes, all append/
    //        drop at the END of the record + one global-header field:
    //        (a) GLOBAL HEADER gains g_playerHmsTotalLast(f32) right AFTER
    //            lastPlayerLevel(u16) and BEFORE the follower count — the live
    //            running total of the PLAYER's base H/M/S, so the per-level
    //            catch-up grant rate survives a save/load. Read gated on
    //            version>=6; a pre-v6 stream inits it from the live player total
    //            on load (no spurious first grant).
    //        (b) the flags byte gains bit 0x20 = fixedStat (the follower has been
    //            detected as a fixed-stat NPC — 2 player level-ups of 0 engine
    //            award). Bits 1/2/4/8/16 unchanged; 0x20 was free (0 in every
    //            v1–v5 save, so the read is version-agnostic).
    //        (c) the per-follower §HMS block DROPS hmsTarget(f32×3) — it is ALWAYS
    //            max(hmsBaseline, hmsBaseline+hmsCumulative) (set that way at
    //            RecomputeHMS :694/:789 and Enroll), so it is recomputed on load,
    //            never stored. v6 per pool writes baseline,skew,cumulative (3 f32,
    //            was 4). AFTER the captured(u8), v6 APPENDS hmsZeroAwardStreak(u8,
    //            the 0-award detector, clamped 0..2), hmsGrantRemainder(f32×3,
    //            fractional per-pool grant carried across levels, clamped 0<=f<1),
    //            and hmsAwardAccum(f32, the detection tally — SERIALIZED so a
    //            save/load between two player level-ups can't wipe the award
    //            evidence and falsely flag a leveling follower). The v5 READER
    //            is KEPT (INVARIANT #12): it reads the OLD 4-f32/pool layout,
    //            DISCARDS the stored target, recomputes it, and defaults the new
    //            fields (fixedStat=false, streak=0, remainder={0,0,0}, accum=0).
    inline constexpr std::uint32_t kRecProgression = 'PRGN';
    inline constexpr std::uint32_t kProgVersion    = 6;   // v6: §HMS fixed-stat grant (drop target, add streak/remainder/playerHms)

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
