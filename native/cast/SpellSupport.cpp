// cast/SpellSupport.cpp -- which spells the board's picker hides because MFO can only cast them by
// the UNANIMATED direct fallback, whatever the situation (marth 2026-10-06: "hide spells that MFO
// cant do properly on the spell list").
//
// ONLY the cases ChooseBuffRoad (cast/BuffRoad.cpp) decides from the SPELL RECORD alone, through the
// SAME helper it calls (BuffAimedLightWhy):
//   * aimed / targeted Light (Magelight; Self Light stays).
// An ambiguous row (several effects mapping to different caster rows) is NOT hidden any more (marth
// 2026-10-06 agreed to show the Flame / Frost / Lightning Cloaks again): it still takes the direct road
// at cast time (ChooseBuffRoad's direct answer), the picker just lists it.
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
        const char* why = nullptr;
        if (!placement) why = BuffAimedLightWhy(a_spell, arch);
        if (!why) return false;
        if (a_reason) *a_reason = why;
        return true;
    }

    // A spell whose NATURE (for an enemy, for an ally, for self) cannot be read from its record
    // (marth 2026-10-06: AUTO does not cast it and asks for a target). The Apocalypse leech curses are the
    // shape: every effect non-hostile and non-detrimental, archetype Script, Aimed delivery, so ClassifySpell
    // says Buff while the author meant a curse for a foe. True when ALL hold:
    //   * a Buff kind (ClassifySpell: every effect non-hostile and non-detrimental, no beneficial Health
    //     effect, so not a heal),
    //   * not a placement (summon / reanimate, BuffPlacementSpell),
    //   * delivery not Self,
    //   * and AT LEAST ONE effect's archetype says nothing about an ally: Script (the record defers to a
    //     Papyrus script), or an enemy-control archetype the record leaves non-hostile (Calm, Frenzy,
    //     Demoralize, Paralysis, TurnUndead; vanilla Pacify / Calm, whose intent is a foe).
    // Every other archetype is a DECLARED ally buff and keeps AUTO's party behaviour: Invisibility (Fade
    // Other), Cloak, armor / ward / ValueModifier fortifies, Light, Rally ... (an earlier cut keyed this on
    // the predicted engine row NoRow / Script and so caught Fade Other, a genuine ally buff: a regression).
    // ClassifySpell and its enum are untouched. Pure form data: any thread.
    bool SpellNatureUndeclared(RE::SpellItem* a_spell) {
        if (!a_spell || a_spell->effects.empty()) return false;
        if (CasterConsent::ClassifySpell(a_spell) != CasterConsent::SpellKind::Buff) return false;
        if (BuffPlacementSpell(a_spell)) return false;
        if (a_spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf) return false;
        using Arch = RE::EffectArchetypes::ArchetypeID;
        for (const auto* eff : a_spell->effects) {
            const auto* mgef = eff ? eff->baseEffect : nullptr;
            if (!mgef) continue;
            switch (mgef->GetArchetype()) {
            case Arch::kScript:
            case Arch::kCalm:
            case Arch::kFrenzy:
            case Arch::kDemoralize:
            case Arch::kParalysis:
            case Arch::kTurnUndead:
                return true;
            default:
                break;
            }
        }
        return false;
    }

    // A Buff-kind, non-placement spell delivered to Self (Inferno / Apocalypse: archetype Script, Self,
    // non-hostile). Its effect lands on the caster, so a rule that aims it at a foe is a mis-set target
    // (CastOn refuses it; the board does not offer "Enemy (gambit target)" for it). Disjoint from
    // SpellNatureUndeclared, which requires a non-Self delivery. Pure form data: any thread.
    bool SelfDeliveredBuff(RE::SpellItem* a_spell) {
        if (!a_spell || a_spell->effects.empty()) return false;
        if (CasterConsent::ClassifySpell(a_spell) != CasterConsent::SpellKind::Buff) return false;
        if (BuffPlacementSpell(a_spell)) return false;
        return a_spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
    }

}
