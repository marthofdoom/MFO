#pragma once
#include <functional>   // std::function -- not in the PCH
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
    // a_peek (the Service.cpp approach PREFETCH only): may post the scan (500 ms repost limit) but never stamps readAt, never clears unread,
    // never logs and never records a refusal, and reposts on the TTL alone; the result is meaningless to the caller. The first ARRIVAL
    // gate starts the visit; the answer it uses may come from an approach scan, bounded by kGuardFresh.
    GuardState GuardCheck(RE::Actor* a_follower, RE::TESObjectREFR* a_src, Road a_road, bool a_onMain, bool a_peek = false);

    // The CALLING THREAD's last non-Clear GuardCheck result and a running count of them (thread_local, no lock). A caller whose call chain
    // passes several gates (StripCorpse -> LootHere / Take) snapshots n before and after to learn that a gate refused, and reads state / guard
    // to say why (Pending = "guard check pending", Seen = "guard sees <guard>").
    // landing = the Pending came from the CORPSE-TRANSFER hold (Takeable, below), not from a guard answer: an item of the candidate's
    // form type is still on its way to the follower (queued to the main thread). Same contract as a pending guard answer: RETRY, never DONE.
    // notYet (INVARIANTS #84) = the Pending came from a source whose inventory SNAPSHOT is not readable yet (never built, or withheld
    // after a write, or the main thread stalled). It is recorded with landing = true: the same contract and the same short retry
    // (the snapshot is built on the next frame), RETRY, never DONE.
    struct GuardRefusal { std::uint32_t n = 0; GuardState state = GuardState::Clear; RE::FormID guard = 0; bool landing = false; bool notYet = false; };
    GuardRefusal GuardRefusalInfo();

    // INTERIM GUARD GATE. TRUE = refuse (a guard sees the follower or a_src, or the answer is not back yet).
    // a_src may be null (a follower-only check, the scan's). Logs `[theft-gate] ... BARRED:guard` rate-limited.
    bool GuardInterim(RE::Actor* a_follower, RE::TESObjectREFR* a_src, Road a_road);

    // CORPSE GUARD WAIVER (marth 2026-11-13, ClickUp 1dfcuabczbg): TRUE = a corpse whose contents carry NO ownership (no owner on the body, no
    // ExtraOwnership on any held copy), looted OUTSIDE a town (the player's location has no LocationTypes::TownAnchor): the engine never calls
    // that theft, so the witness check is skipped for the corpse (StripCorpse's gate and Take's, both). FALSE inside a town (the guard check
    // stays: "Outside of towns") and for any corpse holding one owned copy (every crime gate stays, everywhere). Reads the inventory: worker.
    bool CorpseGuardWaived(RE::Actor* a_follower, RE::TESObjectREFR* a_corpse);

    // TRUE = the player stands in a town (or there is no player): LootNearby keeps its follower-level guard bar for every category. Outside a
    // town the bar moves into the scan's candidate filter so an unowned corpse (CorpseGuardWaived) is not blocked by it.
    bool PlayerInTown();

    // One copy's verdict. nullptr = may be taken, else the reason. a_xList = the copy's extra list (null = a plain copy,
    // owned by the container's owner, if any). Pure read; no guard check, no log.
    const char* TakeBarred(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj,
                           RE::ExtraDataList* a_xList, Road a_road);

    // The same verdict from a copy's VALUES (the snapshot road, INVARIANTS #84): a_copyOwner = that copy's ExtraOwnership owner
    // (InvSnap::Copy::owner; null = a plain copy or no ownership extra, judged by the container's owner).
    const char* TakeBarredOwner(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESForm* a_copyOwner);

    // Read-only: is at least one copy of a_obj in a_src takeable? (the peek paths) Copies already QUEUED out of an actor source (below) do
    // not count. THE LANDING HOLD: FALSE, with a Pending refusal recorded (GuardRefusal::landing), while an item of a_obj's FORM TYPE is
    // still on its way to this follower: his pack does not show it yet, so a decision that counts or compares that kind of item (ammo and
    // potion counts, the gear baseline) would be made on a stale pack. The caller's guard-pending handling retries the visit.
    bool Takeable(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj);
    // A reader of a_src's inventory got "not yet" from InvSnap::Get (INVARIANTS #84): record the Pending refusal (GuardRefusal::notYet,
    // landing = true) so the visit is retried and the source is never marked done, and say it (rate-limited). Every loot reader that
    // walks a SOURCE's snapshot calls this on a null one, then answers "nothing".
    void NoteInventoryNotYet(RE::Actor* a_follower, RE::TESObjectREFR* a_src);

    // The take: moves up to a_count of a_obj from a_src into the follower, ONLY copies TakeBarred allows. A stack with no
    // barred copy is moved by the one plain RemoveItem the callers always made. Returns how many moved (0 = refused).
    // Checks the guard gate first. a_road is a hint (Container becomes Corpse for an actor source).
    //
    // NO SOURCE IS TRANSFERRED ON THE WORKER (INVARIANTS #83, ENGINE_NOTES 0.30 amendments 2026-11-15 and 2026-11-16b): a corpse since
    // fix/corpse-take-main, a container too since fix/worker-writes-main. Read "actor source" below as "any source". The gates
    // (guard, per-copy ownership) are decided here, on the calling thread, exactly as before; the transfer itself is QUEUED and runs on
    // the MAIN thread (MainThread::Post, FormIDs only, the copies re-picked there by the same Analyse), in queue order, one drain per
    // frame (the whole queue, no per-frame cap; an open inventory menu defers it a frame). The return value is then the PLANNED count
    // and the items are in the pack one frame later. Until they land: Takeable / Take subtract the queued copies from what the source still offers,
    // InFlightWeight carries their weight, and Takeable holds same-type decisions (above). Never waits on main. With no live pump
    // (unsupported runtime) the transfer runs on the calling thread as before (said once). Called where MainThread::EngineWritesAllowed()
    // (the steal road, a posted closure) it runs at once.
    std::int32_t Take(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, std::int32_t a_count, Road a_road);

    // ONE NAMED COPY of a source (the copy whose ExtraUniqueID is a_uid; the gem probe's take), on the same queue and under the
    // same rules as Take. The caller has run its gates (guard, TakeBarred); the per-copy bar is re-read on main. a_landed runs on MAIN
    // right after the transfer with the count that moved, only when it is > 0. Returns the planned count (1), or 0 with nothing queued
    // when this call may write in place (MainThread::EngineWritesAllowed: the main thread, or no live pump): the caller keeps its own
    // direct road, which is legal there.
    std::int32_t QueueCopy(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::TESBoundObject* a_obj, std::uint16_t a_uid, Road a_road,
                           std::function<void(std::int32_t)> a_landed);

    // A MAIN-THREAD follow-up that needs the item IN the follower's pack (the equip of a looted piece). TRUE = a queued take of a_obj for
    // a_follower is still in flight and a_onMain was attached to it: it runs on main right after that take lands, only if something moved
    // (else it is dropped, said loudly). FALSE = nothing in flight (the take ran at once, or has landed): the caller runs its own road.
    bool AfterLanding(RE::FormID a_follower, RE::FormID a_obj, std::function<void()> a_onMain);

    // The weight of everything queued to a_follower and not landed yet (FitsCarryWeight adds it: his pack weight does not show it yet).
    float InFlightWeight(RE::FormID a_follower);

    // Drop the queue (revert / load: MainThread::Clear has dropped the posted drain). Logistics::ClearTransientState, after StopPump.
    void ClearTransfers();

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
