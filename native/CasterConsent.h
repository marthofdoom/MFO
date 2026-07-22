#pragma once
#include "PCH.h"

// INFLUENCE, NOT INSERTION. ENGINE_NOTES §0.28, DESIGN.md §4.5c.
//
// Every mechanism that INSERTS a cast roots the follower (packages) or plays no
// animation (CastSpellImmediate). The one path that casts while MOBILE is the
// follower's OWN combat AI -- it strafes, closes, retreats, and casts, because
// that is what combat casters do. MFO cannot insert into that path, but it can
// change ONE decision inside it.
//
// `CombatMagicCaster::CheckStartCast` (vtable index 0x06) is the AI's
// permission bit: "should I cast the spell in my hand right now?" §0.16's
// refused heal was this returning false. Hooking it -- the same write_vfunc
// technique as Targeting, and the same shape DragonWar.dll ships one slot over
// on CheckShouldEquip -- lets MFO answer YES when the follower holds the gambit
// spell and a rule is latched. Everything else stays the AI's: movement, aim,
// hold time, animation, magicka.
//
// This is the magic twin of the act.attack target hook. That one changes WHO
// the AI fights; this changes WHETHER it casts what it holds. Neither replaces
// the AI -- both rewrite the output of a decision it was already making.

namespace MFO::CasterConsent {

    // Install the CheckStartCast hook across the concrete caster vtables. Once,
    // at kDataLoaded. No-op when bCasterHook is off.
    void InstallHook();
    bool IsHooked();

    // MFO wants this follower to cast this spell. While latched, CheckStartCast
    // returns true for that follower+spell. Cleared when the cast fires or the
    // rule stops winning. MAIN THREAD.
    void Want(RE::FormID a_follower, RE::FormID a_spell);
    void Clear(RE::FormID a_follower);
    void ClearAll();

    // Probe instrumentation (§0.28's decisive measurement).
    struct Stats {
        std::uint32_t seen    = 0;   // CheckStartCast calls for a tracked follower
        std::uint32_t vetoed  = 0;   // ...where the AI's own answer was NO
        std::uint32_t forced  = 0;   // ...where MFO overrode NO -> YES
        std::uint32_t latched = 0;
    };
    Stats GetStats();
    void ClearTransientState();

}
