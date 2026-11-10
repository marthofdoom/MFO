#pragma once
#include "PCH.h"

// FOLLOWER SPELL COST PARITY (ClickUp 1dfcuabcr0c; marth 2026-11-09: "if we know the real cost just charge that, then follower
// reductions").
//
// The engine's skill cost curve (id 26505, called from SpellItem::AdjustCost and, for an autocalc spell, per effect) has a
// PLAYER branch and an NPC branch with separate GMSTs; on some lists the NPC curve is the cheaper one, so a follower pays less
// than the player would. ParityCost(follower, spell) is what the PLAYER'S curve would have charged, at the FOLLOWER's own
// skill in the spell's school, then through the follower's OWN kModSpellCost perks (the engine's entry point 0x26, which runs
// AFTER the curve):
//     P = perk( playerFactor(skill) * base )        engine E = perk( npcFactor(skill) * base )
// MFO never needs `base`: the entry point is probed twice to get its affine form perk(v) = a*v + b, so
//     P = a * (x * pf / nf) + b   with   x = (E - b) / a      (x = the NPC-curved, pre-perk cost).
// DIRECT road: the deduct sites charge Cost() = P instead of the engine cost. ANIMATED road: the engine has already charged E;
// OnObservedCast tops up P - E (a concentration spell per second, Tick). Never below 0 magicka. Only for a follower MFO manages.

namespace MFO::CostParity {

    // kDataLoaded, MAIN thread, after Config::Read. Reads the six cost GMSTs, logs both curves and the NPC/player ratio at
    // skill 0/25/50/75/100.
    void StartupCheck();

    // True when the INI is on and the six GMSTs were read.
    bool Active();

    // What the direct-road deduct sites charge for one second (or one cast) of `a_spell` by `a_caster`: P when parity applies to
    // this follower and spell, else exactly the engine cost. Records that MFO billed this (caster, spell) itself, so the
    // animated top-up does not bill the same cast again.
    float Cost(RE::Actor* a_caster, RE::SpellItem* a_spell);

    // WORKER, inside the SpellSink task's PumpTickGate: the engine's AI-fired road just cast `a_spellID` for `a_casterID`. Tops
    // up P - E once (fire-and-forget) or opens a per-second channel (concentration). Skips an MFO-billed direct cast.
    void OnObservedCast(RE::FormID a_casterID, RE::FormID a_spellID);

    // WORKER (the follower sweep): bills the per-second delta of every open animated channel, closes the ended ones.
    void Tick();

    // Forget all per-session memory (kPreLoadGame / revert, via Followers::ClearTransientState).
    void Reset();
}
