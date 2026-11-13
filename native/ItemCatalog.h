#pragma once
#include <RE/Skyrim.h>

#include "iaus/PotionBuff.h"   // the pure kind enum and the effect row (no engine type)

#include <vector>

// The load-order ITEM CATALOG. The MFO.Synthesis patcher classifies every item
// in the user's load order from its REAL record (effects, flags, slots,
// enchantments) and writes Data/SKSE/Plugins/MFO/mfo_items.json. This module
// loads it and answers category questions the runtime cannot reliably guess
// (e.g. a Requiem restore potion whose MGEF archetype the DLL's heuristic
// misreads). Every accessor is keyed by RESOLVED runtime FormID and is a pure
// read; callers fall back to their own heuristic when the catalog is absent or
// silent (the mod still works with no patcher run).
namespace MFO::Catalog {

    // Parse + resolve the catalog. Call once the data handler is ready
    // (kDataLoaded) so LookupForm works. Safe to call again (idempotent).
    void Load();

    bool Loaded();

    // The resource a potion restores per the catalog, or kNone if it is not a
    // catalogued restore potion (caller then uses its archetype fallback).
    RE::ActorValue PotionRestores(RE::FormID a_potion);

    // A CURE potion (CurePoison / CureDisease archetype), from the catalog. These
    // carry no restore ActorValue, so PotionRestores misses them; the cure-poison
    // gambit reads this instead. kNone when uncatalogued (fail-open: needs a
    // patcher run, and the runtime has no reliable cure heuristic to fall back on).
    enum class Cure { kNone, kPoison, kDisease, kBoth };
    Cure PotionCures(RE::FormID a_potion);
    bool CuresPoison(RE::FormID a_potion);    // kPoison or kBoth
    bool CuresDisease(RE::FormID a_potion);   // kDisease or kBoth

    // The beneficial NON-RESTORE effects of a potion by kind (resist / fortify / regen / invisibility / water breathing / cures), with the
    // magnitude and duration, from the patcher's schema-2 `fx` row (feat/buff-potions, ClickUp 1dfcuabd18f). nullptr = the potion has no
    // `fx` row (an uncatalogued form, a schema-1 catalog, or no patcher run): the caller reads the potion's own records instead
    // (logistics/PotionFx.cpp PotionBuffs). A catalogued potion with NO kind has a row with an empty list, which is NOT nullptr: the
    // patcher saw it and it has none (a drug, a plain restore), so the runtime fallback is not asked.
    const std::vector<Iaus::PotionFx>* PotionEffects(RE::FormID a_potion);

    // Arrow-vs-bolt from the record's real flag. Runtime TESAmmo::IsBolt() is
    // unreliable in practice (vanilla arrows report IsBolt()==true here: the fork's
    // IsBolt reads the wrong layout offset), which made the follower reject every
    // arrow; the catalog gets it right. kUnknown when uncatalogued -> the caller
    // (Logistics::AmmoIsBolt) reads the kNonBolt flag through GetRuntimeData().
    enum class Ammo { kUnknown, kArrow, kBolt };
    Ammo AmmoKind(RE::FormID a_ammo);

    // "Special — never auto-loot": quest items, artifacts/unique enchantments,
    // scripted/no-drop items. False when the catalog is absent (fail-open: with
    // no patcher run, MFO loots as it did before).
    bool IsExcluded(RE::FormID a_item);

    // Worn on an amulet/ring slot (for the loot-jewellery gambit).
    bool IsJewelry(RE::FormID a_item);

    // A soul gem record (for the loot-soul-gems gambit). Callers still fall
    // back to As<TESSoulGem>() when the catalog is absent or silent.
    bool IsSoulGem(RE::FormID a_item);
}
