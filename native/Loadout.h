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

    // Seconds since the gambit spell became castable by the follower's own AI
    // -- armed when MFO equips it, AND when a rule first wants a cast the
    // follower could already make (their own spell in hand counts; the AI
    // deserves the same window either way). Huge value when never armed. See
    // Actuation's note on the measurement confound.
    float SecondsSinceEquip(RE::FormID a_actorID);

    // Restart the AI's window. Called after MFO casts for a follower, so the
    // NEXT time that rule wants to fire, their own AI gets another chance --
    // otherwise the grace applies once per session and never again.
    void ArmGrace(RE::FormID a_actorID);

    // TAKE THE SPELL BACK when no rule wants it any more.
    //
    // This is the frequency limiter, and it is structural rather than a timer:
    // MFO controls what is in the follower's hands, and their AI casts what is
    // in their hands. Leaving a heal equipped after the follower is healed is
    // what let one drain ~1000 magicka -- the condition had gone false and the
    // spell was still there. A gambit spell is held only while its rule wants
    // it. No-op when MFO equipped nothing.
    void ReleaseSpell(RE::FormID a_actorID);

    // Start the cast cooldown for this follower: the spell leaves their hand
    // and does not come back until it expires. Called after ANY gambit cast --
    // theirs or MFO's -- because both spend the same pool and the player sees
    // one follower casting either way.
    void StartCooldown(RE::FormID a_actorID);
    bool CoolingDown(RE::FormID a_actorID);

    // Restore a displaced SHIELD. Called from the hit sink -- a shield is worth
    // giving back at the instant something hits them, and not before.
    void OnFollowerHit(RE::FormID a_actorID);

    // Restore a stowed two-handed weapon once its cast is done.
    void Tick();

    // ── FORCED SELF-CAST support (Actuation, SPEC-self-cast-forced) ──────────
    // While MFO channels a forced self-cast, HOLD its stowed weapon / displaced
    // shield back -- Tick's auto-restore (after fTwoHandedDebounce) is what
    // yanked the spell out of the hand mid-animation. Set on each self-cast
    // fire, cleared when the channel ends.
    void HoldStow(RE::FormID a_actorID);
    void EndStowHold(RE::FormID a_actorID);

    void ClearTransientState();

    int PendingRestores();

}
