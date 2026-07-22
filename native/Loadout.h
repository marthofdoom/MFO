#pragma once
#include "PCH.h"

// The equip policy. DESIGN.md §4.5b.
//
// A gambit spell is cast by the FOLLOWER, not on their behalf: `ActorMagicCaster`
// is driven by the animation graph, so `CastSpellImmediate` can never animate
// (ENGINE_NOTES §0.13). Making them cast means putting the spell in a hand --
// and hands are not free.
//
// The off-hand is the pivot. A spell swapped into the off hand costs a
// one-handed fighter nothing: weapon untouched, still fighting, real animation.
// A shield displaced that way is restored WHEN THE FOLLOWER TAKES A HIT, which
// is the only moment a shield matters. A two-handed wielder must stow their
// weapon entirely, so that swap is debounced.
//
// MFO RESTORES WHAT IT DISPLACED. Anything else is "the mod ate my follower's
// gear". The ledger is transient and never persisted -- it describes live
// engine state (INVARIANTS #16).

namespace MFO::Loadout {

    enum class Grip : std::uint8_t {
        Empty,        // nothing in hand -- off-hand equip is free
        OneHanded,    // weapon in the right hand, off hand free or shielded
        TwoHanded,    // greatsword/axe/bow/crossbow -- must be stowed to cast
        Caster,       // already holding a spell -- nothing to do
    };

    struct Hands {
        Grip grip = Grip::Empty;
        RE::TESForm* right = nullptr;
        RE::TESForm* left  = nullptr;
        bool leftIsShield  = false;
        bool alreadyHolding = false;   // the very spell we want to cast
        bool holdingInLeft  = false;   // ...and in which hand, so the cast matches
    };

    // Read what the follower is holding. Pure reads.
    Hands Read(RE::Actor* a_actor, RE::SpellItem* a_spell);

    enum class Ready : std::uint8_t {
        AlreadyReady,   // spell is in hand -- cast now
        Equipped,       // we just put it in hand
        Debounced,      // two-hander, too soon since the last swap
        Failed,
    };

    // Put the spell in a hand if that is allowed right now, recording anything
    // displaced so it can be given back.
    Ready Prepare(RE::Actor* a_actor, RE::SpellItem* a_spell, std::string& a_why);

    // Hand back everything owed to ONE follower -- on dismissal, or when they
    // leave the party still holding MFO's choice.
    void Restore(RE::FormID a_actorID);

    // Rebuild the restore obligation from LIVE state after a load. The ledger
    // is not persisted (#16), so a save taken mid-cast must be undone by
    // looking at what the follower is actually holding.
    void Reconcile();

    // Seconds since MFO put a spell in this follower's hand, or a huge value if
    // it did not. The AI needs a WINDOW to cast before MFO overrides it -- see
    // Actuation's note on the measurement confound.
    float SecondsSinceEquip(RE::FormID a_actorID);

    // Restore a displaced SHIELD. Called from the hit sink -- a shield is worth
    // giving back at the instant something hits them, and not before.
    void OnFollowerHit(RE::FormID a_actorID);

    // Restore a stowed two-handed weapon once its cast is done.
    void Tick();

    void ClearTransientState();

    int PendingRestores();

}
