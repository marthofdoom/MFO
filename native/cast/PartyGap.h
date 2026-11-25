#pragma once
// cast/PartyGap.h -- the PURE half of the party-fight gap hold (anim-residue plan A1; the engine half is cast/PartyGap.cpp).
// Standard types only, so tools/iaus_harness/harness.cpp reads the very same rule as the game.
//
// The rule: a cast is HELD (not made on a direct, unanimated road) when ALL hold: it is the combat table (the party is
// fighting), he has no combat controller of his own, it is a Buff-kind (non-heal) cast, and his controller is the ONLY gate
// the animated claim road is missing (ChooseBuffRoad answered DirectNoCombat). A heal is never held (plan decision D1:
// it keeps the instant road), nor is anything out of combat or with a controller.
namespace MFO::PartyGap {
    [[nodiscard]] constexpr bool Holds(bool a_combatTable, bool a_hasOwnController, bool a_buffKind, bool a_controllerIsTheOnlyMissingGate) {
        return a_combatTable && !a_hasOwnController && a_buffKind && a_controllerIsTheOnlyMissingGate;
    }
}
