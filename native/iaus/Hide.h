#pragma once
// iaus/Hide.h -- G2 LAYER 1: the engine's own verdict "does this foe still have him", READ, never written
// (Docs/research/2026-11/g2-hide-design.md section 2, calculate-detection-re.md section 5). ENGINE SIDE (it takes RE:: types);
// the pure generator only ever sees the Tri it fills into Combatant::detectsMe.
//
// THE READ is TauntProbe.cpp:~193-204 (ServiceP2, "The foe's OWN group") unchanged in shape: ONE read lock on the FOE's
// combat group (RE::CombatGroup::lock, fork CombatGroup.h:108), handle compares and plain field copies inside it, nothing
// nested, nothing resolved or locked inside it. WORKER THREAD ONLY (the Scheduler tick; Candidates::Gather and the
// [stealth-probe] both run there). No engine write, no address, no seat: CombatTarget and CombatGroup are plain members the
// shipped code already reads (fork CombatGroup.h:15-44, :69-109).
#include "PCH.h"
#include "iaus/Core.h"

namespace MFO::Iaus::Hide {

    // What the foe's group holds about one target, copied out under the lock.
    struct Record {
        bool          hasGroup = false;   // the foe has a combat controller with a group this lap
        bool          present = false;    // ...and that group has a CombatTarget record of the follower
        bool          lost = false;       // CombatTarget::Flags::kTargetLost
        bool          known = false;      // CombatTarget::Flags::kTargetKnown
        std::int32_t  detectLevel = 0;    // CombatTarget::detectLevel (+04)
        float         stealthPoints = 0.0f;   // CombatTarget::stealthPoints (+08)
        float         lastKnownStamp = 0.0f;  // CombatTarget::lastKnownTimeStamp (+90), the engine's AI clock, RAW
        std::uint16_t attackerCount = 0;
    };

    // WORKER THREAD. a_followerHandle = a_follower->GetHandle().native_handle().
    [[nodiscard]] Record Read(RE::Actor* a_foe, std::uint32_t a_followerHandle);

    // Layer 1's verdict: does this foe detect the follower?
    //   Unknown = the foe has no combat group this lap (nothing to read; Unknown always passes).
    //   No      = the group has no record of him, or it says lost, or detectLevel <= 0 (hidden from this foe).
    //   Yes     = the record exists, is not lost and detectLevel > 0.
    [[nodiscard]] Tri Verdict(const Record& a_r);

}
