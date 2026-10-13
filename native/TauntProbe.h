#pragma once
#include "PCH.h"

// TAUNT S0 PROBES (Docs/TAUNT-DESIGN.md section 6, ClickUp 86e3g6duw). FULLY PASSIVE:
// no claim, no animation, no engine write, no behaviour read of the result. Gated by
// [Taunt] bTauntProbe (INI-only, default 0) and rate-limited per follower.
//   P1: would the combat taunt idle (0x0B8C73 under CombatIdleStandard_withShield, or
//       0x0B8C71 CombatIdleRegular, both event IdleCombatStart) play for this follower now?
//   P2: for each foe fighting the party, is the follower HELD / LOST / ABSENT in that foe's
//       combat-group targets, how often does the foe switch targets, does it sense the follower?
// Worker only (the Scheduler tick, beside the other per-follower combat probes).
namespace MFO::TauntProbe {

    // One call per serviced follower per lap, from Scheduler::Service. a_ownCombat = the
    // follower's own combat flag this lap. Returns at once when the gate is off.
    void Service(RE::Actor* a_f, RE::FormID a_id, bool a_ownCombat);

    // Revert / load: drop every per-follower and per-foe sample (session state).
    void Reset();

}
