#pragma once
#include "PCH.h"

// OPTIONAL INTEROP with "Follower Leveling System Redone" / "A Fun Way To Level
// Followers" (Nexus 181813, by TrumanGIT). NOT AFFILIATED, and MFO ships and
// bundles NONE of that mod's files -- this tab only appears when that mod is
// installed, and it reads/writes the follower state that mod keeps on disk.
//
// LEGAL/RULES BASIS (reviewed 2026-07-28): this is a clean-hands interop addon.
// Game mechanics (a point raises a skill, five points is a level, perks unlock
// at skill thresholds) are not copyrightable; the on-disk field names and paths
// are functional interface facts we must match to interoperate. Every line here
// is MFO's OWN implementation -- no code was copied from their repo, only the
// behaviour learned from it. Their per-follower JSON is USER save-state, not a
// distributed asset, so writing it is not "modifying their files". Guardrails:
// no bundled content, soft detection, credit + link, and DEFENSIVE write-back
// (preserve their exact format and every field we do not own).

namespace MFO::Leveling {

    // Is that mod loaded in THIS process? Soft, runtime, no ESP/master. When
    // false, MFO never touches any of this and the board tab is hidden.
    void RefreshDetection();
    bool Detected();

    // The twelve skills that mod levels, in board order. `key` is the exact
    // top-level key its PerksAndSpells.json uses (an interface fact).
    struct Skill { RE::ActorValue av; const char* label; const char* key; };
    const std::vector<Skill>& Skills();

    // A read-only view of one follower's leveling state, for the board snapshot.
    struct State {
        bool        has = false;    // a state file exists for this follower
        int         level = 1;
        int         points = 0;     // unspent skill points in their pool
        std::array<int, 12> skill{};   // current skill AV, parallel to Skills()
    };
    // MAIN THREAD. Reads the follower's on-disk state + live skill AVs.
    State Read(RE::Actor* a_follower);

    // ── operations, ALL MAIN THREAD, all no-ops when !Detected() ────────────
    // Raise/lower one skill AV by 1 (live). These are raw +1/-1 (Lower floors
    // only at 0); the caller (board) enforces the allocation rules -- how many
    // points remain, and not refunding below what was spent this session.
    bool RaiseSkill(RE::Actor* a_follower, RE::ActorValue a_skill);
    bool LowerSkill(RE::Actor* a_follower, RE::ActorValue a_skill);

    // Commit a level: costs 5 points, +1 level, +5 Health/Magicka/Stamina,
    // grant any newly-earned threshold perks, and write the state file back
    // (defensively). Returns false if fewer than 5 points remain.
    bool LevelUp(RE::Actor* a_follower);

    // Grant every threshold perk/spell the follower now qualifies for, read
    // from the USER's installed PerksAndSpells.json. Fail-soft on any editorID
    // that does not resolve (needs po3 Tweaks for the editorID map).
    //
    // LIMITATION (to close in a later increment): a perk added to a TESNPC base
    // is NOT saved in the savegame, so MFO-granted perks are lost on reload --
    // the source mod re-applies its own via co-save serialization; MFO does not
    // yet. Plan: re-run this for tracked followers on kPostLoadGame. Spells
    // (Actor::AddSpell) DO persist.
    void ApplyProgression(RE::Actor* a_follower);

}
