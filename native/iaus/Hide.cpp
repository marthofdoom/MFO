// iaus/Hide.cpp -- G2 Layer 1 verdict read (contract and threading in iaus/Hide.h).
#include "iaus/Hide.h"

namespace MFO::Iaus::Hide {

    Record Read(RE::Actor* a_foe, std::uint32_t a_followerHandle) {
        Record r;
        if (!a_foe) return r;
        // The foe's OWN group, a fresh read lock with nothing nested (TauntProbe.cpp ServiceP2, the same read).
        auto* fcc = a_foe->GetActorRuntimeData().combatController;
        if (!fcc || !fcc->combatGroup) return r;
        r.hasGroup = true;
        RE::BSReadLockGuard lk(fcc->combatGroup->lock);
        for (const auto& t : fcc->combatGroup->targets) {
            if (t.targetHandle.native_handle() != a_followerHandle) continue;
            r.present        = true;
            r.lost           = t.flags.any(RE::CombatTarget::Flags::kTargetLost);
            r.known          = t.flags.any(RE::CombatTarget::Flags::kTargetKnown);
            r.detectLevel    = t.detectLevel;
            r.stealthPoints  = t.stealthPoints;
            r.lastKnownStamp = t.lastKnownTimeStamp.timeStamp;
            r.attackerCount  = t.attackerCount;
            break;
        }
        return r;
    }

    Tri Verdict(const Record& a_r) {
        if (!a_r.hasGroup) return Tri::Unknown;
        if (!a_r.present || a_r.lost || a_r.detectLevel <= 0) return Tri::No;
        return Tri::Yes;
    }

}
