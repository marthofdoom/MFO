#pragma once
#include "PCH.h"

// FOLLOWER SPELL COST PARITY (ClickUp 1dfcuabcr0c; marth Q13 2026-11-09: "Cheap option, double it. Make sure as part of
// startup that they are cheaper, if they are double their cost").
//
// The engine's skill cost curve (id 26505, called from SpellItem::AdjustCost) has a PLAYER branch and an NPC branch with
// separate GMSTs, and on some lists the NPC curve is the cheaper one. A follower therefore pays less than the player would.
// MFO's direct casts and the engine's animated casts both charge the engine's CalculateCost, so the fix has to live in
// the engine's own cost path, not in MFO's deduct: the kModSpellCost PERK entry point runs AFTER the curve for every
// road. MFO_CostParityPerk (MFO.esp 0x919) multiplies it by 2.0; its entry condition is HasSpell(MFO_CostParityAbility
// 0x918), so the x2 reaches only a caster that holds the marker ability. MFO adds the ability (Actor::AddSpell) to a
// follower it manages and removes it when he is dismissed or switched off on the board.

namespace MFO::CostParity {

    // kDataLoaded, MAIN thread, AFTER Config::Read and Forms::Resolve. Reads the six GMSTs, evaluates both curves, logs the
    // [costparity] lines, and decides whether the doubling is active for the session.
    void StartupCheck();

    // True when the startup check decided to double (INI on, forms resolved, GMSTs present, NPC curve cheaper at skill 50).
    bool Active();

    // WORKER (the follower sweep). Declare whether this follower should carry the doubling now (want = Active() and MFO
    // is enabled for him). Idempotent: the first call per follower per session, and every change, posts one main-thread
    // job; repeats are a map hit. Never touches a followers list.
    void Sync(RE::FormID a_actorID, bool a_want);

    // Forget the per-session memory (kPreLoadGame / revert, via Followers::ClearTransientState): the ability itself is
    // saved in the .ess, so the next Sync re-checks it on the main thread.
    void Reset();
}
