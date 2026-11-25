#pragma once
#include "PCH.h"
#include "iaus/Snapshot.h"   // AreaActor / AreaPicture: the plain-data part the IAUS snapshot carries

#include <vector>

// iaus/AreaActors.h -- THE MAIN-BUILT TABLE OF NEARBY ACTORS behind the area guard (package M2, placed-spells-plan.md; the rule is iaus/AreaCensus.h).
//
// THREADING (INVARIANTS #84, the EffectMirror / PerkEffects shape): the process lists are resized by the main thread, and Actor::IsHostileToActor walks
// factions, the crime state and the combat groups. So the table is BUILT ON THE MAIN THREAD (BuildPass, inside the posted snapshot pass,
// Diagnostics.cpp) and READ ANYWHERE as an immutable shared_ptr behind a leaf mutex. Read never waits.
// WHAT IS LISTED: every actor of the high and middle-high process lists that has 3D and is not dead, disabled or deleted, the party included (the
// reader leaves out the caster, his foes and his allies by id), with the set of CASTERS it is hostile to. A caster is a follower that asked
// (Want / Read) in the last kWantTtlTicks; hostility is per caster because "bystander" is.
// FRESHNESS: rebuilt every kAreaGuard.refreshTicks while somebody asks. Unlike the perk table it HAS an age gate: a stale table would be a crime, so
// a table older than kAreaGuard.maxAgeTicks, built before the last pause, or built before this caster asked, answers NotReady and the area guard
// closes (AreaCensus.h). It opens again with the next pass; nothing is guessed meanwhile.
//
// NOT serialized. No co-save, ABI, address-id, hook-seat, INI or ESP change.
namespace MFO::AreaActors {

    // ANY THREAD. Keep the table built with a_caster among its casters (a follower in combat, every tick: Diagnostics.cpp). No read.
    void Want(RE::FormID a_caster);
    // ANY THREAD. The actors NOT hostile to a_caster (himself left out) into a_out; asks like Want. Never AreaPicture::NotApplicable.
    [[nodiscard]] Iaus::AreaPicture Read(RE::FormID a_caster, std::vector<Iaus::AreaActor>& a_out);

    void BuildPass(std::uint64_t a_postTick);   // MAIN THREAD, once per worker tick (Diagnostics.cpp posts it with EffectMirror::BuildPass)
    void Clear();                               // revert / load (Serialization.cpp ResetAllState, beside EffectMirror::Clear)
    void LogStats();                            // `[area-guard]`, one line per 60 s

}
