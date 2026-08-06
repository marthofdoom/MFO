#pragma once
#include "PCH.h"

// INFLUENCE, NOT INSERTION. ENGINE_NOTES §0.28, DESIGN.md §4.5c.
//
// Every mechanism that INSERTS a cast roots the follower (packages) or plays no
// animation (CastSpellImmediate). The one path that casts while MOBILE is the
// follower's OWN combat AI -- it strafes, closes, retreats, and casts, because
// that is what combat casters do. MFO cannot insert into that path, but it can
// change ONE decision inside it.
//
// `CombatMagicCaster::CheckStartCast` (vtable index 0x06) is the AI's
// permission bit: "should I cast the spell in my hand right now?" §0.16's
// refused heal was this returning false. Hooking it -- the same write_vfunc
// technique as Targeting, and the same shape DragonWar.dll ships one slot over
// on CheckShouldEquip -- lets MFO answer YES when the follower holds the gambit
// spell and a rule is latched. Everything else stays the AI's: movement, aim,
// hold time, animation, magicka.
//
// This is the magic twin of the act.attack target hook. That one changes WHO
// the AI fights; this changes WHETHER it casts what it holds. Neither replaces
// the AI -- both rewrite the output of a decision it was already making.

namespace MFO::CasterConsent {

    // Install the CheckStartCast hook across the concrete caster vtables. Once,
    // at kDataLoaded. No-op when bCasterHook is off.
    void InstallHook();
    bool IsHooked();

    // MFO wants this follower to cast this spell. While latched, CheckStartCast
    // returns true for that follower+spell -- and, in FORCE mode, false for
    // every OTHER spell (v1.0.28's exclusive control). v1.0.30: the latch is
    // deliberately NOT cleared when the cast fires. It used to be, and that
    // opened a between-casts LEAK: for the whole cast cooldown -- up to a full
    // service interval times the party size before the next Want() -- the deny
    // was off and the AI slipped its own spell in between two gambit casts.
    // The latch now spans the cast AND its cooldown, so suppression is
    // continuous.
    //
    // v1.0.32: the latch holds for the COMBAT'S DURATION once armed. The
    // v1.0.30 lifetime still dropped it on the scheduler's H3 release -- a
    // SINGLE service tick where the cast condition momentarily didn't hold --
    // and the engine's combat casters tick many times inside one service
    // interval, so a condition flicker opened a sub-tick window the AI used to
    // slip its own spell in. The release points are now ONLY: combat end, the
    // follower's dismissal, and revert/load (never past combat end -- the
    // scheduler's non-combat branch clears every serviced follower). MAIN
    // THREAD.
    void Want(RE::FormID a_follower, RE::FormID a_spell);
    void Clear(RE::FormID a_follower);
    void ClearAll();

    // The gambit spell this follower is latched to cast, or 0 if unlatched. The
    // scheduler reads it to drive the CASTER combat stance (CombatStyle) and its
    // magicka-dry -> melee fallback. Locked shared read; safe off the job-worker
    // tick, cheap when nothing is latched (the map is empty).
    RE::FormID WantedSpell(RE::FormID a_follower);

    // COOLDOWN-CONSULTED PERMIT (v1.0.32). fCastCooldown used to gate only the
    // EQUIP debounce (Loadout::Prepare) -- the thunk's force-YES had no
    // cooldown consult, so while the spell stayed in the follower's hand the
    // permit fired every caster tick and casts BURST (deck: 4 in ~2.2s).
    // Loadout::StartCooldown now stamps the next-allowed-cast time onto the
    // latch entry itself (under the hook's own lock), and the combat-thread
    // thunk DENIES the wanted spell until it is due -- the deny of OTHER
    // spells is unaffected; exclusivity is separate from pacing. Deny and
    // permit are two dials on one latch: deny spans the rule's lifetime, the
    // permit opens once per cooldown. The pace dies with the latch (a fresh
    // fight's first cast is prompt). No-op for an unlatched follower. Written
    // from the task queue (every StartCooldown call site), read from the
    // combat thread -- the thunk never calls Loadout's non-atomic maps.
    void NoteCooldown(RE::FormID a_follower, float a_seconds);

    // OUR spell just fired -- the [cast] sink watched the gambit's own spell
    // leave the follower's hands (AI-driven or package-forced). v1.0.30: this
    // does NOT clear the latch (see Want above); it retires the PER-CAST
    // transients that must not outlive the cast they described:
    //  * the miss flag -- consumed, or a stale "AI cast a different spell"
    //    would short-circuit the NEXT grace window into an instant re-force
    //    (with the package declined, that loop is a silent cast every service
    //    tick -- the pacing the cooldown exists to prevent);
    //  * the deny-log dedup entry -- reset, so the FIRST denied own-cast of
    //    the NEXT cooldown window logs again: the once-per-cycle line that
    //    proves the gap stays closed, at cast cadence, never tick cadence.
    // Returns whether the follower was actually latched, so the caller can log
    // the hold transition without taking a second lock. MAIN THREAD.
    bool NoteOurCast(RE::FormID a_follower);

    // THE MISS DETECTOR (hybrid forced-cast). The [cast] sink reports every
    // spell a tracked follower fires; while a follower is LATCHED, a cast of a
    // DIFFERENT spell is the AI overruling the gambit -- Marcurio casting his
    // own Chain Lightning instead of the configured Firebolt. That swap also
    // re-equips and RESETS the grace clock, so without this flag the grace
    // window never elapses and the miss is invisible to Actuation. NoteCast is
    // called from the sink's queued task (the same serialized SKSE task queue
    // the evaluator tick drains, plus the map's own lock); OtherCastSeen is
    // read by Actuation on the next tick and is erased with the latch by
    // Clear/ClearAll -- no flag outlives the rule that armed it.
    void NoteCast(RE::FormID a_follower, RE::FormID a_spell);
    bool OtherCastSeen(RE::FormID a_follower);

    // Probe instrumentation (§0.28's decisive measurement).
    struct Stats {
        std::uint32_t seen    = 0;   // CheckStartCast calls for a tracked follower
        std::uint32_t vetoed  = 0;   // ...where the AI's own answer was NO
        std::uint32_t forced  = 0;   // ...where MFO overrode NO -> YES
        std::uint32_t latched = 0;
    };
    Stats GetStats();
    void ClearTransientState();

}
