#pragma once
// logistics/PotionFx.h -- THE BENEFICIAL, NON-RESTORE EFFECTS OF A POTION, AND DRINKING ONE NAMED POTION (feat/buff-potions,
// ClickUp 1dfcuabd18f). The kinds and the one rule are in iaus/PotionBuff.h; this file is their engine side.
#include "PCH.h"
#include "iaus/PotionBuff.h"

#include <vector>

namespace MFO::Logistics {

    // The beneficial effects of a_potion by kind (resist, fortify, regen, invisibility, water breathing, cures): CATALOG FIRST (the
    // MFO.Synthesis patcher's schema-2 `fx` row, Catalog::PotionEffects: a catalogued potion's answer stands, even when it is empty, so a
    // drug the patcher saw stays empty), else the SAME rule read off the potion's own records at run time (ClassifyRuntime in PotionFx.cpp,
    // written to match the patcher's ClassifyFx in installer/MFO.Synthesis/Catalog.cs). Empty for a food, a poison, or any potion with a
    // Hostile or Detrimental effect (a drug: PotionIsDrug). Reads the form only; any thread.
    std::vector<Iaus::PotionFx> PotionBuffs(RE::AlchemyItem* a_potion);

    // IAUS looting (marth 2026-11-13: "Potions shoudl also read all effects, in fact food and drink as well"): the buff kinds of ANY
    // non-poison, non-drug ALCH, food and drink included -- the catalog row first, else the runtime rule PotionBuffs uses. Worker-safe
    // (reads the record only). logistics/LootNeed.cpp.
    std::vector<Iaus::PotionFx> ConsumableBuffs(RE::AlchemyItem* a_item);

    // Drink the carried potion a_potion (FormID) NOW: ActorEquipManager::EquipObject, the same road DrinkBest takes (the AlchemyItem equip
    // path is the vanilla "drink"; v8 equip authority governs only ARMO / WEAP / AMMO / LIGH, and MFO files no ch.30 potion allowance, so
    // nothing refuses it: MAP.md "DRINK ROAD"). Refuses (false, nothing consumed): he cannot act (Actuation::CannotActReason), the form is
    // not a carried potion with a catalogued or readable buff effect, or this potion is still inside its own duration (no chain-drinking
    // the stack). Worker domain, like DrinkPotion.
    bool DrinkPotionForm(RE::Actor* a_follower, RE::FormID a_potion);

    // Forgets the per-potion drink gates (ClearTransientState).
    void ClearBuffDrinks();
}
