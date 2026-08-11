#pragma once
#include "PCH.h"

// PACKAGE ACTUATION. DESIGN.md §4.5c -- THE RULING: a PACKAGE *is* the action.
//
// Every mechanism that merely APPLIES an effect was refuted in the field and
// is recorded as such (ENGINE_NOTES §0.10-§0.13): CastSpellImmediate on all
// four casting sources, Projectile::LaunchSpell, DoCombatSpellApply, sending
// animation events, and driving MagicCaster by hand (wedges at state 1). All
// silent. Equipping a spell and letting the follower's own AI cast DOES
// animate -- but it is AI-DISCRETIONARY, and it declined a weak heal every
// time it was measured.
//
// What is left is the thing vanilla itself does: put a PACKAGE on the actor
// for the duration of the action. MFO owns the ACTION, never the FOLLOWER.
//
// ── THE ROUTE, and why it is the only one ────────────────────────────────
//
//   MFO_CommandQuest (0x80A, priority 60, start-game-enabled, in the SEQ)
//     -> alias 0 "MFO_CommandActor", flags Optional|AllowReuse|AllowReserved
//        -> ALPC = MFO_CastPackage (0x820), riding vanilla UseMagic 000504F5
//     -> the follower is FILLED INTO alias 0
//        -> the engine builds ExtraAliasInstanceArray on the actor
//        -> Actor::CheckForCurrentAliasPackage() (vfunc 0x049) finds it
//        -> the package runs
//
// This is not a clever trick -- it is vanilla's own follower system. Verified
// against the shipped data rather than assumed: DialogueFollower (000750BA)
// alias 0 "Follower" carries ALPC PlayerFollowerPackage (0005C84B), its fill
// type is Conditions (empty until runtime), and Simple Follower Framework's
// DialogueFollowerScript.psc does `pFollowerAlias.ForceRefTo(FollowerActor)`
// on recruit and `.Clear()` on dismiss AND NOTHING ELSE. Filling instances
// the packages; clearing removes them. That is the whole mechanism.
//
// ── WHY NOT THE ALTERNATIVES ─────────────────────────────────────────────
//
// The ALYSLC package-stack route (`packageStackMap`, `kCombatOverride`) that
// ROADMAP M9 step 3 still describes DOES NOT EXIST at the pinned rev
// c4ab853d -- zero hits across the whole include/ and src/ tree. It is not
// merely inadvisable, it is unreachable through this library. That is
// INVARIANTS #64 landing on our own roadmap text, and it means the ALPC route
// and the stack route were never two options; there is one.
//
// PapyrusUtil AddPackageOverride is banned by INVARIANTS #18/#19: overrides
// persist through saves and ClearPackageOverride removes OTHER MODS' entries.
//
// ── WHY THIS IS ADDITIVE, at three layers (DESIGN §4.6) ──────────────────
//
//   1. DELIVERY.    MFO adds its OWN alias instance to the actor's
//                   ExtraAliasInstanceArray. NFF, Inigo and DialogueFollower
//                   keep theirs untouched. There is no shared list to stomp.
//   2. ARBITRATION. Which alias package wins is decided by QUEST PRIORITY, an
//                   engine-arbitrated number. MFO is 60; vanilla's mode is 30
//                   and DialogueFollower is 50. We never raise our own number
//                   to win an argument -- see DeclineReason::Contention.
//   3. RELEASE.     Clearing MFO's alias removes exactly MFO's package. The
//                   #18 hazard (a call whose contract is wider than the
//                   caller's intent) cannot arise here.
//
// kAllowReserved (0x200) on the alias is what makes this work at all on
// framework followers: they are already reserved by their own mod's quest
// alias, and without that flag our fill would silently no-op.
//
// ── THE HARD CONSTRAINTS THIS MODULE OBEYS ───────────────────────────────
//
//   * NEVER write TESQuest::refAliasMap. It is the RESULT of a fill, not the
//     fill. Hand-writing state a flow owns is exactly INVARIANTS #16.
//   * NEVER write AIProcess::currentPackage. ActorPackageData is opaque at
//     the pin -- there is no bound way to build one.
//   * NEVER touch TESNPC::defaultPackList or TESAIForm::aiPackages. Those are
//     BASE RECORDS, shared by every instance of that NPC. That is the stomp
//     this whole architecture exists to avoid.
//   * NEVER EvaluatePackage(_, a_resetAI = true) in combat. ALYSLC
//     field-proved that clears the combat group and the next hit does ZERO
//     damage. We pass (true, false), always.
//
// ── ASYNCHRONY IS THE SHAPE OF THIS MODULE ───────────────────────────────
//
// The fill goes through the VM, and VM dispatch is ASYNCHRONOUS -- the alias
// is NOT filled when the call returns. So there is no success/failure to read
// from a dispatch. The state machine below is therefore driven entirely off
// OBSERVATIONS of engine state, never off a return value:
//
//     Idle -> Requested -> Filled -> Running -> Done -> Idle
//              (dispatched)  (fill    (package  (package
//                             seen)    seen)     gone)
//
// Pump() makes those observations. Nothing else advances the phase.
//
// ── ONE HOLDER AT A TIME, and that is forced, not chosen ─────────────────
//
// There is ONE MFO_CastPackage record and ONE alias 0. TESPackage carries a
// refCount and instances are SHARED -- mutating the Spell input while another
// follower is mid-cast changes the spell under them. Both facts point the
// same way, so this module tracks a single holder. Per-follower actuation
// needs the per-verb records at 0x821+ and is not this commit.

namespace MFO::Packages {

    // P7 MULTI-FOLLOWER LOOT EXCURSIONS. Up to this many followers may loot-travel
    // AT ONCE, one per SLOT. The loot quest (Forms::g_lootQuest) ships 8 aliases =
    // 4 (actor, target) PAIRS, interleaved: slot k uses actor alias 2*k (carries
    // slot k's travel package) and target alias 2*k+1 (the DLL-filled destination).
    // Slot 0 = aliases 0/1, byte-identical to the shipped single-slot route. The
    // claim model per slot is UNCHANGED (static priority 60, claim at alias fill,
    // release by eviction). The RETREAT quest is a SEPARATE, still-single holder.
    inline constexpr int kMaxLootSlots = 4;

    // The observed lifecycle of one commanded action. Advanced ONLY by Pump()
    // reading engine state -- see the asynchrony note above.
    enum class Phase : std::uint8_t {
        Idle,        // nothing outstanding
        Requested,   // ForceRefTo dispatched; fill NOT yet observed
        Filled,      // alias fill observed; package not yet running
        Running,     // MFO's package observed as the actor's current package
        Done,        // package ended; awaiting release
    };

    const char* PhaseName(Phase a_phase);

    // Why a request was refused. Every one of these is a DECISION MFO made and
    // is logged with the actor named -- §5.3, a rule that could not run says
    // why, it is never silent.
    enum class Decline : std::uint8_t {
        None,
        Disabled,      // bUsePackages is off (the default)
        NoRecord,      // MFO_CommandQuest / MFO_CastPackage did not resolve
        NoVM,          // the VM is unreachable, so no fill is possible
        QuestStopped,  // the command quest is not running -- SEQ missing?
        Busy,          // another follower already holds the single alias slot
        Contention,    // a higher-or-equal-priority quest owns the alias layer
        BadInputs,     // the package's data inputs could not be located by name
        SelfRoute,     // cast_self barred from the package route -- see Begin()
    };

    const char* DeclineName(Decline a_reason);

    struct Status {
        Phase         phase        = Phase::Idle;
        RE::FormID    holder       = 0;      // whose action this is, 0 when idle
        RE::FormID    spell        = 0;      // what we asked them to cast
        float         phaseSeconds = 0.0f;   // time in the CURRENT phase
        float         totalSeconds = 0.0f;   // time since the request
        bool          fillObserved = false;  // alias 0 currently resolves to holder
        bool          packageLive  = false;  // MFO's package is their current one
        std::uint32_t requests     = 0;      // session counters, for the #53
        std::uint32_t completions  = 0;      // heartbeat -- a subsystem whose
        std::uint32_t declines     = 0;      // correct behaviour is invisible
        std::uint32_t timeouts     = 0;      // MUST publish one
    };

    // Is the mechanism usable at all right now? False means every request
    // below declines, and the caller must fall back rather than assume.
    bool Available();

    // Command a follower to cast a spell ON THEMSELVES.
    //
    // CURRENTLY ALWAYS DECLINES (Decline::SelfRoute, v1.0.32) -- deliberately.
    // targType 6 alone is field-proven (probe 6), but probe 6's record carried
    // NO QNAM; the shipped MFO_CastPackage carries an AUTHORED QNAM (its
    // authored Target is t4 -> alias 1), and writing t6 into a QNAM-carrying
    // record at runtime is the rev-4 CTD's surviving zero-precedent cell
    // (§0.20/§0.22 -- the crash was re-explained as the QNAM, not the t6).
    // Until a dedicated probe clears QNAM+t6, cast_self misses fall through
    // to Actuation's silent apply, exactly the pre-v1.0.32 behaviour.
    //
    // When it is eventually armed: Target input = PTDA **targType 6**, value 0.
    // NOT targType 4 -> alias 0 -- an alias indirection back to the DELIVERING
    // alias is FIELD-REFUTED (§0.19, #65): it stalls, owning the actor while
    // resolving nothing.
    Decline CastSelf(RE::Actor* a_follower, RE::SpellItem* a_spell);

    // A CONCENTRATION HOLD (v1.0.53 deck freeze -> bounded streams). A
    // fire-and-forget package cast ends itself: the [cast] sink observes the
    // release and Pump lets go one linger later. A CONCENTRATION cast has no
    // release to observe -- the stream runs for as long as the package owns
    // the actor -- so the caller states the bound UP FRONT and Pump enforces
    // it: release when holdSeconds elapse after the stream is observed, OR
    // earlier when the healWatch actor's health tops off (heal streams), OR
    // earlier when a teammate crosses the caster->target line (ffWatch,
    // hostile streams -- Sightline::TeammateInFireLine each tick). Every
    // release also InterruptCasts the stream so the beam dies with the
    // package. holdSeconds == 0 (the default) is the unchanged
    // fire-and-forget lifecycle.
    struct CastHold {
        float      holdSeconds = 0.0f;   // 0 = fire-and-forget (linger release)
        RE::FormID healWatch   = 0;      // release early when this actor tops off
        bool       ffWatch     = false;  // release early on a teammate in the line
    };

    // Command a follower to cast at another actor.
    //
    // SHAPE (field-proven, probe 5): fill ALIAS 1 with the victim, and the
    // package's Target input is PTDA **targType 4 -> alias 1**, with QNAM
    // naming the command quest. This is Bethesda's own
    // CWFinaleLeaderExecuteEnemyLeader pattern -- follower in alias A attacks
    // actor in alias B -- and it is how a gambit aims at a runtime choice.
    //
    // Preferred over writing a live ObjectRefHandle into targType 0: that route
    // is also proven (probes 1-3) but names the actor at author time, so it
    // cannot express "whoever the rule picked this tick".
    Decline CastAt(RE::Actor* a_follower, RE::SpellItem* a_spell,
                   RE::TESObjectREFR* a_target, const CastHold& a_hold = {});

    // Is MFO's bounded concentration stream for exactly this follower+spell
    // live right now? Callable from ANY thread (a single relaxed atomic --
    // the consent hooks consult it from the engine's caster threads, where
    // the single-writer holder state may not be read). True from the moment
    // a CastHold request is accepted until its release; the consent hooks
    // allow the AI channel for a concentration spell ONLY while this is true,
    // so the bounded package stream passes and everything else is denied.
    bool StreamLive(RE::FormID a_actor, RE::FormID a_spell);

    // Observe engine state and advance the phase. MAIN THREAD, once per tick.
    // This is the ONLY function that moves the state machine.
    void Pump();

    // Release the alias if this actor holds it. Idempotent.
    //
    // #55 ("restore before you stop tracking") binds harder here than it does
    // for spells: alias fills are SERIALIZED INTO THE .ess by the engine, so a
    // follower left latched comes back latched on the next load, forever. The
    // dismissal path must call this BEFORE the record leaves the active set.
    void Release(RE::FormID a_actorID);

    // The Claim-and-Release pop's completion signal: the [cast] sink observed
    // a_actorID fire a_spellID. If that is the current holder casting the
    // commanded spell, Pump releases him one short linger later instead of
    // waiting for the package's own Cooldown cycle or the hard timeout --
    // §0.23's "the action must then END and hand him back", made real. Called
    // from the sink's queued task (the same serialized queue Pump runs on);
    // a no-op for anything but the live holder+spell pair.
    void NotifyCast(RE::FormID a_actorID, RE::FormID a_spellID);

    // Unconditional release. Revert, kPreLoadGame, and shutdown.
    void ReleaseAll(const char* a_why);

    // Mint the session's EVICTION MARKER (#48): the non-actor XMarker that
    // release-by-eviction force-fills into the loot/retreat ACTOR aliases in
    // place of the player -- forcing the PLAYER into a package-carrying alias
    // yanked him out of furniture. MAIN THREAD ONLY (PlaceObjectAtMe); call
    // from kPostLoadGame/kNewGame, where the player is in-world. Idempotent --
    // one marker per session. Until it runs, evictions fall back to the player.
    void EnsureEvictMarker();

    Status Get();

    // ── OPTION A: engine-pathed loot travel (DESIGN behaviour layer) ─────────
    // Fill MFO_LootQuest's SLOT a_slot aliases so the engine WALKS a_follower to
    // a_ref via the vanilla Travel package; the caller (Logistics) transfers on
    // arrival. Up to kMaxLootSlots followers travel concurrently, one per slot
    // (a_slot in [0, kMaxLootSlots): actor alias 2*a_slot, target 2*a_slot+1).
    // Logistics owns the follower->slot map and passes the owning slot down every
    // call. Returns false if bLootTravel is off, the records are unresolved, off
    // AE, or the quest is not running.
    //
    // The fill is SAVE-SERIALIZED (#55): the caller MUST LootTravelClear() on
    // arrival, interrupt (combat), or timeout. ReleaseAll clears every slot on
    // load/revert/shutdown, so even a missed clear self-heals on the next load.
    bool LootTravelFill(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, int a_slot);
    // A LEG BOUNDARY inside an ongoing batch excursion: retarget the follower to
    // the next loot ref (refill this slot's TARGET alias only) WITHOUT releasing
    // -- priority and the ACTOR alias are untouched, so no framework hand-back /
    // turn-around. See .cpp.
    bool LootTravelRetarget(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, int a_slot);

    // a_follower is optional: pass it to re-evaluate that follower immediately
    // (drops travel this tick); omit and the priority drop still frees them on
    // the engine's next evaluation. Release is by EVICTION of a_slot's ACTOR alias
    // (force-fill the eviction marker), NOT by clearing the sticky, unclearable
    // alias -- see the .cpp.
    void LootTravelClear(const char* a_why, RE::Actor* a_follower = nullptr, int a_slot = 0);

    // Evict a_id from the loot alias IF he currently occupies ANY slot's actor
    // alias (keyed to OCCUPANCY across all slots, not the live intent -- the alias
    // is never emptied, so a follower dismissed any time after his last travel
    // still holds it). Priority can't free a dismissed follower (nothing reclaims
    // him); this force-fills his slot's actor alias with the eviction marker.
    // No-op if a_id holds no slot. See the .cpp.
    void LootTravelEvictIf(RE::FormID a_id);

    // ── RETREAT PROBE: travel-to-PLAYER under kIgnoreCombat ─────────────────
    // One question, answered in natural play: can an alias Travel package
    // carrying kIgnoreCombat (0x00100000) pull a follower AWAY from a live
    // combat controller? MFO_RetreatQuest is the loot quest's exact claim
    // model -- STATIC priority 60, claim at fill, release by eviction, never a
    // priority flip (ENGINE_NOTES 0.36) -- with alias 1 filled with the PLAYER
    // and a Travel package that does NOT yield to combat. The Scheduler drives
    // it (fill once per combat per low-confidence far follower) and instruments
    // every tick; this module owns the alias plumbing only.
    //
    // The fill is SAVE-SERIALIZED like the loot fill (#55): the Scheduler MUST
    // RetreatClear on combat end / arrival / timeout, and ReleaseAll evicts the
    // retreat alias on load so a mid-retreat save self-heals.
    bool RetreatFill(RE::Actor* a_follower);
    void RetreatClear(const char* a_why, RE::Actor* a_follower = nullptr);
    // Who currently holds the retreat alias (0 when nobody), and for how long.
    RE::FormID RetreatHolder();
    float      RetreatSeconds();
    RE::NiPoint3 RetreatStartPos();   // where she was when the retreat began (movement proof)
    // Evict a_id from the retreat alias if he occupies it -- the dismissal-path
    // twin of LootTravelEvictIf, same #55 tail: the fill is engine-serialized
    // and nothing reclaims a dismissed follower.
    void RetreatEvictIf(RE::FormID a_id);

    // Session-scoped counters, cleared on revert like every sibling module.
    void ClearTransientState();

}
