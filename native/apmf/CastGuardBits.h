#pragma once
// apmf/CastGuardBits.h -- the PURE half of the ch.25 cast guard claim (apmf/CastGuard.cpp): which CastGuardFlags the follower's ONE guard claim
// carries this lap, and which of them a refusal strikes. Standard types only, so tools/iaus_harness/harness.cpp runs the very same answers as the game.
// Harbinger does not unite the bits of several guard claims (the best basis wins whole), so every bit MFO wants rides the one claim.
// To add a guard: a bit here (static_assert-ed against APMF_API.h in apmf/CastGuard.cpp), a term in Want, and its place in Strike.
#include <cstdint>

#include "scheduler/Rank.h"

namespace MFO::CastGuardBits {

    inline constexpr std::uint32_t kLineOfFire       = 1u << 0;
    inline constexpr std::uint32_t kChannel          = (1u << 1) | (1u << 2);
    inline constexpr std::uint32_t kBystanders       = 1u << 3;
    inline constexpr std::uint32_t kSelfBlastParty   = 1u << 4;
    inline constexpr std::uint32_t kSelfBlastSummons = 1u << 5;
    inline constexpr std::uint32_t kSelfBlast        = kSelfBlastParty | kSelfBlastSummons;

    inline constexpr std::uint32_t kBystandersMinAbi = 32;   // an older APMF REFUSES the whole claim for the bit
    inline constexpr std::uint32_t kServeMinAbi      = 35;   // CanServe: the only way to ask for the self blast bits (no ABI bump)

    struct In {
        std::uint32_t abi             = 0;
        bool          lineOfFire      = false;   // bFriendlyFireHold (and the caster hook on)
        bool          channelBound    = false;
        bool          selfBlast       = false;   // an IAUS self-blast claim of his stands (BlastStands)
        bool          summonsBlock    = false;   // bAreaSummonsBlock
        bool          selfBlastServed = false;   // Harbinger accepted the self blast bits in a CanServe dry run this session
        std::uint32_t refused         = 0;       // the bits struck this session
    };

    // Is the dry run worth asking (the ONE moment the bits could be wanted)? False on an APMF without CanServe: today's bits.
    [[nodiscard]] constexpr bool AsksServe(const In& a) { return a.selfBlast && a.lineOfFire && a.abi >= kServeMinAbi; }

    [[nodiscard]] constexpr std::uint32_t Want(const In& a) {
        std::uint32_t w = 0;
        if (a.lineOfFire) {
            w |= kLineOfFire;
            if (a.abi >= kBystandersMinAbi) w |= kBystanders;
            if (AsksServe(a) && a.selfBlastServed) w |= kSelfBlastParty | (a.summonsBlock ? kSelfBlastSummons : 0u);
        }
        if (a.channelBound) w |= kChannel;
        w &= ~a.refused;
        if ((w & kSelfBlastParty) == 0) w &= ~kSelfBlastSummons;   // Harbinger refuses the summons bit alone
        return w;
    }

    // What a refusal of a_want strikes for the session: the self blast bits ALONE when they were asked (never LineOfFire with them), else LineOfFire and
    // Bystanders (one arming rule), else everything asked.
    [[nodiscard]] constexpr std::uint32_t Strike(std::uint32_t a_want) {
        if (a_want & kSelfBlast) return kSelfBlast;
        constexpr std::uint32_t kArmed = kLineOfFire | kBystanders;
        return (a_want & kArmed) ? (a_want & kArmed) : a_want;
    }

    // Does an IAUS self-blast claim stand on the hand? a_lockRank = the hand lock's owner, a_blastLock = the lock names a self blast, a_claimLive = the
    // offense claim on that hand stands. A gambit that took the hand (a table row's rank) reads false.
    [[nodiscard]] constexpr bool BlastStands(bool a_blastLock, int a_lockRank, bool a_claimLive) {
        return a_blastLock && a_claimLive && Scheduler::Rank::IsIaus(a_lockRank);
    }

}
