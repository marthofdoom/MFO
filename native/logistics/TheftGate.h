#pragma once
// logistics/TheftGate.h -- the UNIVERSAL THEFT GATE (ClickUp 1dfcuabczbg, SEV-1 user report: follower looting in Whiterun
// gave the player 5 g theft bounties and the guards attacked). RE: Docs/research/2026-11/theft-and-crime-alarm-re.md.
//
// Three rules, every MFO take road (loose, flora, container, corpse, swap-up, gear, LOTD) passes the first two:
//   1. BAR ALL STOLEN (marth 2026-11-11): a follower never takes an item copy owned by anyone other than null / the
//      player / PlayerFaction / that follower's own side (base NPC chain or a faction he is in), from any source, including
//      stolen goods the player dropped. Per copy: that copy's ExtraOwnership, else the container's GetOwner().
//   2. NO LOOTING IN FRONT OF GUARDS (marth 2026-11-11, interim until the culprit is found): no take of any kind while a
//      live guard can see the follower or the item. One function, GuardInterim, so it is easy to find and to remove.
//   3. (doors / furniture are the Errand code's: ErrandRest.cpp DoorLocked, ErrandSmith.cpp FindStations.)
// Data-only reads: no new engine address id (the ids IsAnOwner / IsOffLimits-by-actor are NOT used here).
//
// Threads: Take / Takeable / TakeBarred run on whichever thread the take road runs on (the worker, as the loot code does);
// GuardInterim answers from a cache and posts the main-thread guard scan (highActorHandles + the sightline are main-thread
// reads, INVARIANTS #72); until the first answer lands the verdict is BARRED (fail closed, never fail open).
namespace MFO::Logistics::TheftGate {

    enum class Road : std::uint8_t { Loose, Flora, Container, Corpse, Swap, Gear, Lotd, Door, Guard,
                                     Steal, Pick };   // batch L: act.steal's take, act.pick_locks' unlock (ring entries only)
    const char* RoadName(Road a_road);

    enum class GuardState : std::uint8_t { Clear, Seen, Pending };   // Pending = no fresh answer yet: RETRY, never blocklist
    // Full answer. a_onMain = the caller is on the main thread already (scans now, so it is never Pending).
    GuardState GuardCheck(RE::Actor* a_follower, RE::TESObjectREFR* a_src, Road a_road, bool a_onMain);

    // INTERIM GUARD GATE. TRUE = refuse (a guard sees the follower or a_src, or the answer is not back yet).
    // a_src may be null (a follower-only check, the scan's). Logs `[theft-gate] ... BARRED:guard` rate-limited.
    bool GuardInterim(RE::Actor* a_follower, RE::TESObjectREFR* a_src, Road a_road);

    // One copy's verdict. nullptr = may be taken, else the reason. a_xList = the copy's extra list (null = a plain copy,
    // owned by the container's owner, if any). Pure read; no guard check, no log.
    const char* TakeBarred(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj,
                           RE::ExtraDataList* a_xList, Road a_road);

    // Read-only: is at least one copy of a_obj in a_src takeable? (the peek paths)
    bool Takeable(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj);

    // The take: moves up to a_count of a_obj from a_src into the follower, ONLY copies TakeBarred allows. A stack with no
    // barred copy is moved by the one plain RemoveItem the callers always made. Returns how many moved (0 = refused).
    // Checks the guard gate first. a_road is a hint (Container becomes Corpse for an actor source).
    std::int32_t Take(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, std::int32_t a_count, Road a_road);

    // LOOSE REF: LooseRefBarred (any owner, off-limits, ...), and at ARRIVAL (a_arrival) the guard gate too. nullptr = may be taken.
    // a_road Loose or Flora. Any owner is barred, which is stricter than the engine's own pickup test (IsAnOwner needs a new
    // address id, so it is not bound: tier A).
    const char* LooseBarred(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, Road a_road, bool a_arrival, bool* a_pending = nullptr);
    // a_pending (arrival): set when the only reason is a guard answer not back yet; the caller skips the tick and must NOT travel-fail.

    // The proof line for an OWNED copy that moved on the steal road (Steal::TakeOwnedContents), not through Take: `via=steal-road`.
    void LogOwnedCopy(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, std::int32_t a_count, const char* a_how);

    // The per-follower ring of the last 3 MFO world actions; printed on every [crime-probe] BOUNTY UP.
    void NoteAction(RE::FormID a_follower, Road a_road, RE::FormID a_ref, const char* a_result);
    void LogRings();
}
