#pragma once
// iaus/MoveOrder.h -- THE ORDER a movement assignment becomes (tight-movement-plan.md 6.3 and 11 item 1): mode, point, leash, arrival. PURE, no caller in the DLL yet.
//
// The modes are Harbinger's own (APMF_API.h MoveMode, ABI 48: Free 0, Stand 1, GoTo 2, Kite 3, Disengage 4, Path 5). The pass says WHO STANDS WHERE, this says
// what the engine is asked for. Free = no order: the engine fights as it does today. Path is in the vocabulary (the pass emits single points, so it emits none).
//
// TO ADD OR CHANGE
//   which mode a spot kind asks for ...... kOrderRows (one table, its ORDER is the precedence when a place is good for several kinds)
//   a radius or a rule of the stay half .. t.order* (Core.h Tuning)
//   a cap a kind needs ................... Spots.h kSpotKinds needs + Spots.cpp AllowedKinds
#include "iaus/Spots.h"

namespace MFO::Iaus::Move {

    struct OrderRow { SpotKind kind; MoveMode mode; bool pin; };   // pin: held at t.orderLeashPin instead of t.orderLeash
    inline constexpr OrderRow kOrderRows[] = {
        { SpotKind::RetreatExit,  MoveMode::Disengage, false },
        { SpotKind::Kite,         MoveMode::Kite,      false },
        { SpotKind::Regroup,      MoveMode::GoTo,      false },
        { SpotKind::Flank,        MoveMode::GoTo,      true  },
        { SpotKind::Cover,        MoveMode::Stand,     true  },
        { SpotKind::Chokepoint,   MoveMode::Stand,     true  },
        { SpotKind::Firing,       MoveMode::Stand,     false },
        { SpotKind::HighGround,   MoveMode::Stand,     false },
        { SpotKind::HealReach,    MoveMode::Stand,     false },
        { SpotKind::Peel,         MoveMode::Stand,     false },
        { SpotKind::ReachUnreach, MoveMode::Stand,     false },
        { SpotKind::Stay,         MoveMode::Free,      false },   // StayMode decides between Free and a Stand on his own place
    };
    static_assert([] { std::uint32_t seen = 0; for (const OrderRow& r : kOrderRows) seen |= 1u << static_cast<unsigned>(r.kind); return seen == ((1u << kSpotKindCount) - 1u - (1u << static_cast<unsigned>(SpotKind::LootRoute))); }(), "every spot kind a fight offers has an order row");

    [[nodiscard]] inline const OrderRow& OrderRowOf(std::uint32_t a_kinds) {
        for (const OrderRow& r : kOrderRows) if (a_kinds & KindBit(r.kind)) return r;
        return kOrderRows[std::size(kOrderRows) - 1];
    }

    // He stays. Left to the engine, or held where he is? marth: melee may fight one enemy freely when that is best, hold otherwise. PLACEHOLDER lines (t.order*).
    [[nodiscard]] MoveMode StayMode(const PartyPicture& a_pic, const Member& a_m, const RankedAction& a_act, const Tuning& a_t);
    // The order of one assignment. a_m.order is the one that stands: an order that says the same keeps its point and its gen.
    [[nodiscard]] MoveOrder OrderOf(const PartyPicture& a_pic, const Member& a_m, const Assignment& a_as, const Tuning& a_t);
}
