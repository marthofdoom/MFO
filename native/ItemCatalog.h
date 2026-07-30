#pragma once
#include <RE/Skyrim.h>

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

    // "Special — never auto-loot": quest items, artifacts/unique enchantments,
    // scripted/no-drop items. False when the catalog is absent (fail-open: with
    // no patcher run, MFO loots as it did before).
    bool IsExcluded(RE::FormID a_item);

    // Worn on an amulet/ring slot (for the loot-jewellery gambit).
    bool IsJewelry(RE::FormID a_item);
}
