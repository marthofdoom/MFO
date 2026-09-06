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

    // ── intelligent hand selection (marth's hand policy, 2026-09-06) ───────────
    // WHICH hand(s) a cast should claim, decided from the follower's REAL, LIVE
    // loadout + perks + magicka pool -- never a class/build guess (the same
    // governing rule DESIGN §4.5b already holds this whole file to). This is a
    // PURE decision: no engine writes, no APMF dependency (this module knows
    // nothing about APMF -- see the header banner above). A caller executes it.
    //
    // HEALS ARE NOT DECIDED HERE. A heal is LEFT, always, regardless of
    // anything below -- marth's hard, non-negotiable rule (the deck-proven
    // failure: a cast took the right hand while the follower was momentarily
    // unarmed, then an equip gambit's own re-equip displaced it ~500ms later).
    // See APMFBridge::kApmfHandLeft / ClaimHealCast's doc. A heal caller never
    // needs PlanCastHand at all.
    enum class HandPick : std::uint8_t {
        Left,        // a weapon owns the right hand (or will, imminently) --
                     // claim LEFT ONLY, leave the weapon hand's own combat
                     // equip AI untouched. Never rip a weapon out to force a
                     // spell (the same "compose, don't substitute" rule the
                     // weapon-hand-exclusion HARD RULE encodes).
        DualCast,    // BOTH hands genuinely free, ONE spell wanted, and the
                     // follower has the REAL dual-casting perk for that
                     // spell's school AND can afford the doubled cost from
                     // CURRENT magicka -- claim BOTH hands for the SAME
                     // spell, empowered. NOTE: today's kIntent_Cast claim
                     // shape (APMF_CastRequest) carries exactly ONE hand hint
                     // per claim -- this outcome is a DECISION, not yet an
                     // executable claim; see APMFBridge.h's ClaimHealCast doc
                     // and CAST-DELIVERY.md for what is missing to fire it.
        EitherFree,  // both hands free, no dual-cast (no perk, or can't
                     // afford it) -- the caller may claim either hand for
                     // THIS spell. When a second, DIFFERENT spell is also
                     // wanted the same tick (the "juggle" case), give that
                     // one the OTHER hand -- PlanCastHand only ever plans ONE
                     // spell's hand at a time; orchestrating two is the
                     // caller's job (it alone knows what else is wanted).
    };

    // a_weaponHandActive: true iff a weapon currently owns (or an equip
    // gambit is about to reassert into) a_actor's right hand -- callers
    // combine a live Read(a_actor, nullptr).grip check with their own
    // "an equip claim is about to re-arm" signal (the APMF-owned path uses
    // APMFBridge::WeaponHandActive, which does exactly that combination; the
    // legacy hybrid path already IS this signal via its own Read() call in
    // Actuation.cpp). Passing false when a weapon claim is actually pending
    // reopens the exact race the hard rule exists to close -- get this bit
    // right before calling.
    HandPick PlanCastHand(RE::Actor* a_actor, RE::SpellItem* a_spell,
                          bool a_weaponHandActive);

    // Does a_actor have the REAL dual-casting perk for a_spell's OWN magic
    // school (read from its costliest effect's Magic Skill, the same read
    // Logistics_Cast.cpp's TargetMagicSchool uses), AND enough CURRENT
    // magicka to afford the doubled cost? Pure read (HasPerk + the base
    // template's own perk list + CalculateMagickaCost) -- reflects exactly
    // what the follower has, never a class/build assumption (the standing
    // "player agency over rigid classes" rule). false for a non-school spell,
    // an unreadable effect, or a follower/spell that is null.
    bool CanDualCast(RE::Actor* a_actor, RE::SpellItem* a_spell);

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

    void ClearTransientState();

    int PendingRestores();

}
