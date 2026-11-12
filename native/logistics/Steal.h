#pragma once
// logistics/Steal.h -- THE ONE WITNESS PRE-CHECK behind act.steal (card G13) and the OWNED-lock path of act.pick_locks
// (card G14). RE: Docs/research/2026-11/theft-and-crime-alarm-re.md sections 2, 5, 6.
//
// THE RULING (marth 2026-11-02 / 11-11): followers never commit a DETECTED crime. Pre-check every crime-shaped action; any hit
// means the take does not happen; there is never a refund after the fact; pickpocket is cut. "A crime is the issue": success is
// zero `[crime-probe] BOUNTY UP` from a steal or a pick (CrimeProbe, bCrimeGoldProbe, is the evidence tool).
//
// WHY BOTH THE FOLLOWER AND THE PLAYER (RE section 2 / 5): after the teammate-to-player redirect inside StealAlarm the engine
// judges the witnesses FOR THE PLAYER. So a steal is allowed only when ALL of these hold at the moment of the take:
//   1. the follower is sneaking (card G1 act.sneak state, Actor::IsSneaking);
//   2. TheftGate::GuardCheck is Clear (no guard sees the follower or the item);
//   3. every loaded, live, non-teammate, non-player actor (animals included) within the witness radius, max(GMST iCrimeAlarmRecDistance,
//      Harbinger's hearing default 1536), read by name at run time, never a constant, has ALL senses 0 toward the FOLLOWER AND ALL senses
//      0 toward the PLAYER (APMFBridge::SenseOf with that radius for sight and hearing; the redirected crime is judged for the player and
//      detection includes sound). A radius past Harbinger's maximum refuses (fail closed);
//   4. the item's OWNER (an NPC base, or any member of an owning faction), when loaded, does not see the follower or the player,
//      even beyond the alarm range.
// Any unreadable answer (Harbinger below ABI 18, a sight ray deferred or unavailable, an unresolved actor) is NOT "clear": it is
// Pending (retry) or Refused. Fail closed, never open. No new detection model: SenseOf + Sightline + TheftGate are the readers.
//
// THREADS. SenseOf and the sight rays are TRUE MAIN THREAD ONLY. Check() is callable from the worker: it answers from a cache
// and posts the main-thread evaluation (the TheftGate::GuardCheck pattern); CheckNow() is for a caller already on the main
// thread (the posted take / unlock write re-checks there, so nothing can change between the proof and the act).
//
// Log: `[steal] F=<fol> item=<base>(<name>) owner=<id|0> witnesses=N verdict=take|REFUSED:<who/why>`, rate-limited 30 s per
// (follower, ref, verdict). Every verdict that gates a world action also writes a TheftGate ring entry (Road::Steal / Road::Pick),
// so a `[crime-probe] BOUNTY UP` names the last three MFO world actions.
#include "TheftGate.h"

namespace MFO::Logistics::Steal {

    enum class Verdict : std::uint8_t { Take, Refused, Pending };   // Pending: no usable answer yet, RETRY (never blocklist)

    // WORKER or any thread. a_ref = the loose item (Road::Steal) or the locked/owned container (Road::Pick). a_why (optional)
    // receives the reason of a Refused answer. Posts the main-thread evaluation when no fresh answer is cached.
    Verdict Check(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, TheftGate::Road a_road, std::string* a_why = nullptr);

    // MAIN THREAD ONLY. Evaluate now. Take / Refused are final; Pending = a sight measurement was deferred this frame (the pair
    // is queued for the pump), the caller does NOT act.
    Verdict CheckNow(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, TheftGate::Road a_road, std::string* a_why = nullptr);

    // LOOSE ITEM for act.steal (Category::Steal). The SOURCE bars LooseRefBarred has, MINUS the owner bars (that is the point of the
    // action): a LOTD museum display, player storage / a player home, a quest item, a faction's vendor / stolen-goods / evidence
    // container, and anything the player's own side owns (the player, PlayerFaction): a follower never "steals" from the player.
    // a_arrival false = the scan (source bars only, no witness read); true = the arrival: also Check (Road::Steal).
    // nullptr = may be taken. a_pending: set when the only reason is that the pre-check has no answer yet (retry next tick,
    // never a travel-fail, never a blocklist). The returned text is valid until the caller's next call on this thread.
    const char* LooseBarred(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, bool a_arrival, bool* a_pending = nullptr);

    // ── OWNED CHESTS for act.pick_locks (Q-L-6, marth 2026-11-12: "pick + steal contents") ──
    // A Thief-line follower's act.pick_locks may pick an OWNED locked chest (never a door) when Check passes, and then takes its
    // loot-kind contents (the kinds act.steal takes, Q-L-1: coin, lockpicks, valuable misc, soul gems, ingredients, jewellery, drinkable
    // potions; never equipment or ammo) as a steal. Normal looting is untouched: it still never takes an owned copy.
    // Owned = an explicit owner or an off-limits (cell-owned) container.
    bool IsOwnedChest(RE::TESObjectREFR* a_chest);
    // The source bars for an owned chest (the owner bars of the theft gate give way to the witness check, these do not): a LOTD
    // drop-off / display, player storage, a quest container, a faction's vendor / stolen-goods / evidence container, and a chest owned
    // by the player's own side or the follower's. nullptr = may be picked and stolen from. Worker or main thread (data-only reads).
    const char* OwnedChestBarred(RE::Actor* a_follower, RE::TESObjectREFR* a_chest);

    enum class OwnedTake : std::uint8_t {
        Posted,    // the main-thread take is in flight: do nothing else this tick, ask again next tick
        Retry,     // a sight measurement was deferred (or the guard answer is not back): nothing more moved, ask again next tick
        Took,      // finished: at least one owned copy moved
        Nothing,   // finished: there was no owned loot-kind copy to take
        Cut,       // stopped: the witness check refused (or a source bar, or the chest relocked): nothing more moves, there is no refund
    };
    // WORKER (the arrival). Drives one posted main-thread take per follower. The take re-runs CheckNow on the main thread right before
    // each RemoveItem batch (one item form), so nothing changes between the proof and the act. a_why receives a Cut / Retry reason.
    OwnedTake TakeOwnedContents(RE::Actor* a_follower, RE::TESObjectREFR* a_chest, std::string* a_why = nullptr);
    void ClearOwnedTakes();   // revert / load
}
