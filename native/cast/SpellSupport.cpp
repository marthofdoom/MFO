// cast/SpellSupport.cpp -- which spells the board's picker hides because MFO can only cast them by
// the UNANIMATED direct fallback, whatever the situation (marth 2026-10-06: "hide spells that MFO
// cant do properly on the spell list").
//
// ONLY the cases ChooseBuffRoad (cast/BuffRoad.cpp) decides from the SPELL RECORD alone, through the
// SAME helpers it calls (BuffAmbiguousRowWhy / BuffAimedLightWhy):
//   * an ambiguous row (several effects mapping to different caster rows),
//   * aimed / targeted Light (Magelight; Self Light stays).
// A target-location (rune-shaped) buff is NOT hidden: only a SELF cast of it goes direct (BuffSelfRuneWhy);
// at an ally it takes the claim road, so it is situational.
// Situational fallbacks (no recipient, self-concentration of a non-Self spell, no combat controller,
// Harbinger absent or ABI < 19) are NOT here: they depend on the fight, not the record.
// HOSTILE target-location spells (Fire Rune ...) are NOT hidden: CastOn's ownedCast has no delivery
// filter, so an Offense rune takes the animated ClaimOffenseCast road (see Docs / the task log).
//
// Pure form data (ClassifyArchetype is cached under a leaf mutex): safe on any thread.
#include "Actuation_internal.h"
#include "CasterConsent.h"

namespace MFO::Actuation {

    bool CastRoadUnsupported(RE::SpellItem* a_spell, const char** a_reason) {
        if (a_reason) *a_reason = nullptr;
        if (!a_spell) return false;
        // ChooseBuffRoad answers NotBuff for any other kind: those roads have their own rules.
        if (CasterConsent::ClassifySpell(a_spell) != CasterConsent::SpellKind::Buff) return false;
        const auto delivery = a_spell->GetDelivery();
        // A concentration buff of a non-Self delivery keeps its own claim road (DirectTarget.cpp): not static.
        if (delivery != RE::MagicSystem::Delivery::kSelf &&
            a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration)
            return false;
        const bool placement = BuffPlacementSpell(a_spell);
        // ChooseBuffRoad keys the row with seat 0's self flip for any non-Self buff (ally recipient or
        // explicit self); a Self-delivery spell has self=1 either way.
        const auto arch = ClassifyArchetype(a_spell, /*a_seat0=*/delivery != RE::MagicSystem::Delivery::kSelf);
        const char* why = BuffAmbiguousRowWhy(arch);
        if (!why && !placement) {
            why = BuffAimedLightWhy(a_spell, arch);
        }
        if (!why) return false;
        if (a_reason) *a_reason = why;
        return true;
    }

}
