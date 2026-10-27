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
    // a_equipOrder (T#75): true iff this stance is an EQUIP ORDER -- an
    // act.equip_melee / act.equip_ranged gambit won (or stands satisfied as)
    // the hand claim this tick. While an equip order owns the stance, the
    // equip gate below DENIES the follower's combat AI re-arming spells or
    // staves over the forced weapon, so the weapon STICKS instead of MFO and
    // the AI trading it every service tick. Deliberately NOT set by the #65
    // class override or the caster magicka-dry melee fallback: those are style
    // PICKS, not weapon orders, and must not mute the AI's own magic. A later
    // Want with a_equipOrder=false (a cast latch flipping the stance) releases
    // the hold; Clear releases it with the stance.
    //
    // a_allowDual (dual-wield gate, marth 2026-10-27 "perk/class gated"): only meaningful for Stance::Melee.
    // true = the follower's perks vote dual wield, so the melee stance writes MFO_MeleeStyle (kAllowDualWielding);
    // false (default) writes MFO_MeleeSingleStyle, which does not allow it. Stored with the desire; a flip is a
    // style handoff in ApplyTick.
    void Want(RE::FormID a_follower, Stance a_stance, bool a_equipOrder = false, bool a_allowDual = false);

    // Drop this follower's stance ownership -- combat end / dismissal / revert.
    // The live CSTY already reverted when the per-combat controller was
    // destroyed; this only clears MFO's bookkeeping so the next fight re-
    // baselines against a fresh controller. MAIN / worker thread. Idempotent.
    // Clears the COMBAT-TABLE stance only: a standing substrate-cast bit (below)
    // or a pending restore keeps the entry, so the per-OOC-lap teardown can run
    // unconditionally (a stale combat stance and its equip claim still go).
    void Clear(RE::FormID a_follower);
    void ClearAll();

    // OOC SUBSTRATE CAST OWNERSHIP (feat/mfo-substrate-followup, review F1). An
    // apmf/Substrate.cpp cast pair owns the cast stance (MFO_CastStyle) on the
    // Harbinger substrate controller through its OWN bit, never through the
    // combat table's `stance`: the two never overwrite each other. ApplyTick's
    // substrate-only call applies the cast style iff this bit is set (and
    // ignores the table stance); its real-combat call ignores the bit. Locked
    // map writes, no controller touched: worker / main thread, NEVER under
    // apmf's g_subMx (this takes CombatStyle's own mutex).
    //
    // a_seq (re-review of 9bdd6fa): the PAIR's own sequence number (apmf/Substrate.cpp, a monotonic counter
    // stamped at open). Opens run on the worker AND the main thread (a self recipient, SummonOnMain), and every
    // CombatStyle call is deferred outside g_subMx, so a late call for an ENDED pair can land after a NEWER
    // pair's Want: an older seq never overwrites a newer one, and a Clear whose seq is not the stored one is a
    // no-op.
    void WantSubstrateCast(RE::FormID a_follower, std::uint64_t a_seq);
    // Drops ONLY the bit (iff a_seq is the stored one); the entry is erased iff no combat-table stance remains.
    // a_restore (review F2, the reason=real-combat close): when the bit's cast
    // style is what stands on the controller and no table stance owns it, arm a
    // restore -- the next ApplyTick on THAT controller writes the saved engine
    // style back (a table Want / WantSubstrateCast arriving first cancels it; a
    // different controller drops it, the old style died with the old one).
    void ClearSubstrateCast(RE::FormID a_follower, std::uint64_t a_seq, bool a_restore = false);

    // T#76: release the stance ownership IFF it is currently an EQUIP ORDER. The
    // equip-gambit stance clamp's duration is now tied to the gambit's CONDITION
    // (held while true, released when it goes false) rather than "until battle
    // end" -- the scheduler calls this on every combat tick where no equip gambit
    // held, so a mage whose equip gambit went false reverts to its own/auto
    // stance and can cast again. A cast-latch stance, the #65 class override, and
    // the magicka-dry melee fallback are NOT equip orders and are left untouched
    // (they keep their own hold-until-battle-end contract). Idempotent (no owner,
    // or a non-equip-order owner -> no-op). MAIN / worker thread.
    void ReleaseEquipOrder(RE::FormID a_follower);

    // T#76: does this follower CURRENTLY hold an equip-order stance? The scheduler
    // uses it to PRESERVE the melee/ranged clamp across a tick where a HIGHER-
    // priority rule stopped the gambit scan before the equip rule (an ordered
    // cast/heal fires): the equip gambit's truth is unknown that tick, so the
    // hold must persist rather than be overwritten by the class override / cast
    // latch -- a mage left-hand-casts the ordered spell (gate-exempted) while the
    // dagger stays force-held, and reverts only when the equip gambit is KNOWN
    // false. MAIN / worker thread (takes the same g_mx as ReleaseEquipOrder).
    bool HoldsEquipOrder(RE::FormID a_follower);

    // Apply / re-assert the owned stance on the follower's live controller.
    // COMBAT THREAD ONLY -- called from the UpdateCombat hook with the very
    // controller the engine just handed this actor. Saves the engine-derived
    // style once per fight, writes the MFO CSTY, and re-asserts if the engine
    // re-derives under it (the cast probe measured that it sometimes does).
    // a_substrateOnly: the actor's controller is a Harbinger substrate (no real
    // fight) -- only the substrate-cast bit is applied, as Stance::Cast.
    void ApplyTick(RE::Actor* a_actor, RE::CombatController* a_cc, bool a_substrateOnly = false);

    // Lock-free fast-path gate for the hook: true iff any follower currently
    // owns a stance. Keeps the common no-op combat tick off the mutex.
    bool AnyActive();
    std::size_t OwnedCount();   // diagnostics

    // ── THE EQUIP GATE (T#75) ─────────────────────────────────────────────────
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

    // HARBINGER CLIENT MODE (fix/mfo-gates-client-only): with Harbinger present the
    // gate above is not installed, and an equip order is expressed as Harbinger's
    // ch.15 kIntent_Equipment claim, whose t2a seat denies the same spell/staff
    // re-arms. A LEDGER hold (cast/Equip.cpp g_forcedWeapon) is claimed by
    // ReconcileForcedWeapon; THIS covers the rest: an order held on a weapon the AI
    // itself equipped ("already holding that category", no ledger). Claims or
    // refreshes naming the weapon in hand, and releases once the order is gone
    // (Clear releases too). Call once per combat service lap, AFTER
    // ReconcileForcedWeapon and ReleaseEquipOrder. Worker. No-op when Harbinger is
    // absent (the gate is installed then).
    void ServiceOrderClaim(RE::Actor* a_follower);

}
