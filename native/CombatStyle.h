#pragma once
#include "PCH.h"

// COMBAT-STYLE OWNERSHIP -- the weapon twin of CasterConsent's cast control.
//
// The problem (deck, Auri, v1.0.32): MFO's act.equip_melee gambit forces the
// mace into an archer's hands, but her OWN combat AI -- weighted ranged -- re-
// draws the bow the instant the equip's suppression window lapses. MFO and the
// engine ping-pong over the weapon and she never attacks: "switch back and
// forth without doing anything." A one-shot EquipObject cannot win against a
// ranged-forward combat style.
//
// The fix mirrors the cast machinery: don't fight the AI's weapon choice per
// tick -- change the STYLE that drives it. While a follower's winning gambit is
// an equip, MFO swaps the LIVE per-combat CombatController::combatStyle (offset
// 0x38, below the §0.29 AE divergence -- never the base record) to a stance-
// matched MFO CSTY. Melee gambit -> MFO_MeleeStyle (bow starved, avoidThreat 0,
// so the AI closes and swings); ranged gambit -> MFO_RangedStyle. The stance
// follows the winning EQUIP gambit, NOT the follower's class, so "make my mage
// melee" / "make my mage shoot" need no special case.
//
// WHY THIS IS SAFE where driving from the evaluator is not: the swap is applied
// on the ENGINE'S COMBAT THREAD, from inside Character::UpdateCombat (the same
// hook Targeting rides, vtable 0xE4) -- the one place a live controller is
// handed to us every combat tick for EVERY combatant, melee or ranged (the
// caster hook barely fires for an archer). The evaluator/scheduler runs on a
// job worker (§0.30) and only ever WRITES THE DESIRE into a locked map here; it
// never touches a CombatController. "Hold until battle end" is free: combatStyle
// lives on the per-combat controller, so the swap dies when the controller is
// destroyed at combat end; Clear just drops the stale bookkeeping.

namespace MFO::CombatStyle {

    // Cast = the pure-mage caster style (MFO_CastStyle) driven by cast gambits;
    // the scheduler flips a dry caster to Melee until magicka returns.
    enum class Stance : std::uint8_t { None = 0, Melee = 1, Ranged = 2, Cast = 3 };

    // The winning equip gambit wants this follower in this stance. Locked map
    // write; touches no CombatController, so it is safe from the job-worker
    // scheduler tick. No-op when the feature is off or the stance is None.
    // Persists until Clear (battle end) or the next Want (melee<->ranged
    // handoff) -- exactly "hold until battle end or another equip triggers".
    //
    // a_equipOrder (#75): true iff this stance is an EQUIP ORDER -- an
    // act.equip_melee / act.equip_ranged gambit won (or stands satisfied as)
    // the hand claim this tick. While an equip order owns the stance, the
    // equip gate below DENIES the follower's combat AI re-arming spells or
    // staves over the forced weapon, so the weapon STICKS instead of MFO and
    // the AI trading it every service tick. Deliberately NOT set by the #65
    // class override or the caster magicka-dry melee fallback: those are style
    // PICKS, not weapon orders, and must not mute the AI's own magic. A later
    // Want with a_equipOrder=false (a cast latch flipping the stance) releases
    // the hold; Clear releases it with the stance.
    void Want(RE::FormID a_follower, Stance a_stance, bool a_equipOrder = false);

    // Drop this follower's stance ownership -- combat end / dismissal / revert.
    // The live CSTY already reverted when the per-combat controller was
    // destroyed; this only clears MFO's bookkeeping so the next fight re-
    // baselines against a fresh controller. MAIN / worker thread. Idempotent.
    void Clear(RE::FormID a_follower);
    void ClearAll();

    // Apply / re-assert the owned stance on the follower's live controller.
    // COMBAT THREAD ONLY -- called from the UpdateCombat hook with the very
    // controller the engine just handed this actor. Saves the engine-derived
    // style once per fight, writes the MFO CSTY, and re-asserts if the engine
    // re-derives under it (the cast probe measured that it sometimes does).
    void ApplyTick(RE::Actor* a_actor, RE::CombatController* a_cc);

    // Lock-free fast-path gate for the hook: true iff any follower currently
    // owns a stance. Keeps the common no-op combat tick off the mutex.
    bool AnyActive();
    std::size_t OwnedCount();   // diagnostics

    // ── THE EQUIP GATE (#75) ─────────────────────────────────────────────────
    // The CSTY swap above is a score BIAS, not a prohibition: MFO_MeleeStyle
    // starves magic at 0.1x, but a spell-heavy caster's magic score can still
    // beat her weapon's, and the engine RE-DERIVES the style mid-combat (the
    // [wstyle] breadcrumb measured it) -- windows in which her own combat AI
    // re-arms a spell, strips the forced weapon, and MFO's equip rule fires
    // again next tick: the deck's Ebony-Tanto thrash. Suppression windows are
    // a band-aid; the real fix is the same influence-not-insertion move as
    // CheckStartCast one layer down: CombatInventoryItem::CheckShouldEquip
    // (vtable 0x0F) is the combat AI's "should I put this in my hands?"
    // permission bit. While an equip order owns a follower's stance, the gate
    // answers NO for spell/staff inventory items (the hand-competing magic),
    // so there is nothing to revert to -- the follower commits to the weapon.
    // The one exemption is the LATCHED gambit spell (CasterConsent::
    // WantedSpell): a spellsword list's cast rule below a satisfied equip is a
    // legal off-hand loan (GAMBIT_FLOWS §7.2) and stays castable. Scoped
    // exactly to the equip-order flag: a cast-gambit stance, the #65 class
    // override, and the magicka-dry melee fallback never engage it, and it
    // releases with the stance (combat end / dismissal / a cast latch flipping
    // the stance / revert). Install once at kDataLoaded; VR-guarded like every
    // other vtable hook (indices unverified there).
    void InstallEquipGate();

}
