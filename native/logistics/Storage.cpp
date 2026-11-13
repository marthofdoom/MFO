// logistics/Storage.cpp -- OFF-CLASS STORAGE (ClickUp 86e3t212u). TEMPLATE ONLY (2026-11-13).
//
// Every body below is a TODO skeleton. NOTHING in this file is called from a live path: no hook, no sink, no tick, no
// errand stop, no co-save record. It compiles so the author who fills it in starts from agreed signatures. The design,
// the co-save byte layout, the threading table, the log lines, the self-test cases and the ORDERED PLAN with file:line
// seats are in Docs/research/2026-11/offclass-storage-design.md. Read that first; fill the steps in its order.
//
// RULES THE AUTHOR MUST KEEP (CLAUDE.md):
//   * co-save: 'STOR' is a NEW independent record. Readers for every shipped version forever (#12), every count bounded
//     (#11), every FormID ResolveFormID'd with an unresolvable one DROPPED (#8), no 0xFF id written (#9). Snapshot under
//     the mutex, write from the copy (never a lock across WriteRecordData).
//   * threading: g_followers is worker-only; the table here has its own LEAF mutex; MainThread::Post is the only road to
//     the main thread; nothing native mutates an inventory off screen (the VM RemoveItem road, Papyrus.h).
//   * no list-specific data: the chest and the home are found at runtime from ownership and record flags.
//   * followers never commit detected crimes: only a container in a cell the PLAYER owns, never a locked town door.
//   * never mask a failure: a move that did not land is logged loudly and the entry is dropped or kept by rule, never retried silently.
#include "PCH.h"
#include "Storage.h"

#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Storage {

    namespace {
        // #11 bounds of the proposed record (writer and reader use the same ones).
        [[maybe_unused]] constexpr std::uint32_t kMaxHomes         = 16;
        [[maybe_unused]] constexpr std::uint32_t kMaxSavedFollowers = 4096;   // the FLWR / PGIV bound
        [[maybe_unused]] constexpr std::uint32_t kMaxItemsPerFollower = 8;    // one per Bucket, with room

        // The table. LEAF mutex: never held across an engine, Harbinger, Papyrus or WriteRecordData call.
        std::mutex                                          g_mx;
        std::vector<Home>                                   g_homes;    // SAVED
        std::unordered_map<RE::FormID, std::vector<Item>>   g_stored;   // SAVED: follower -> his pieces
    }

    // ── the pure decision ───────────────────────────────────────────────────────────────────────────────────────
    StoreOutput DecideStore(const StoreInput& /*a_in*/) {
        // TODO(author), design sections 2 + 3, in this order (first refusal wins, `why` names it):
        //   1. !homeReady                                   -> Sell "no home stop on this trip"
        //   2. bucket None / classBit 0 (shield: kShield)   -> Sell "not a stored kind"
        //   3. classesOn && currentSet && (classBit & currentSet) -> Sell "in his class now" (an in-class reject is just inferior)
        //   4. PLAN TO USE: classesOn ? (classBit & plannedSets) : perkRanks > 0;  else Sell "no class he can take uses it"
        //   5. TOP OF BUCKET: rank > bestOtherRank, or equal and !tieLoses; else Sell "a better one of its kind is carried or stored"
        //   6. ECONOMY: useValue = Pricing::Apply(value, buyMult), saleValue = Pricing::Apply(value, sellMult);
        //      Store only when useValue - saleValue >= minMargin and useValue > saleValue; else Sell "sale beats later use".
        return {};
    }

    bool DecideRetrieve(const RetrieveInput& /*a_in*/, const char** a_whyOut) {
        // TODO(author), design section 4: (currentSet == 0 || (classBit & currentSet)) && roleKeeps && rank > bestCarriedRank.
        // *a_whyOut = classChanged ? "class change" : "better than what he carries".
        if (a_whyOut) *a_whyOut = "";
        return false;
    }

    // ── live seats (uncalled) ───────────────────────────────────────────────────────────────────────────────────
    bool Enabled() {
        // TODO(author): Config::g_offClassStorage && Config::g_townErrands && APMFBridge::ErrandSupported().
        return false;
    }

    void LearnHomeOnMain() {
        // TODO(author), design section 1. MAIN thread. Player's parent cell owned by the player (RefInPlayerStorage's test,
        // LootTake.cpp) and no Home for this cell yet: walk the cell's references, keep a container that is persistent,
        // base TESObjectCONT without CONT_DATA kRespawn, not locked, not IsLOTDDropOff / IsFactionServiceContainer, owner
        // null / player / PlayerFaction; pick the one with the fewest stacks (lowest FormID on a tie); record
        // {chest, cell, TownAnchor}. Log "[storage] home chest CHOSEN ..." or "[storage] no usable chest in ...".
    }

    void NoteWorkerTick() {
        // TODO(author): compare the player's cell FormID with the last one seen (an atomic); on a change Post LearnHomeOnMain.
    }

    Prep Prepare(RE::Actor* /*a_follower*/, RE::FormID /*a_playerTown*/) {
        // TODO(author), design section 3 "the stop". Off -> Prep::Off. A Home whose town == a_playerTown: the follower in
        // its cell -> Ready (dest = the chest ref); else Rest::ShopDoorFor(cell, pos, &lockedId, &level): Found -> Ready
        // (dest = the cell, pos = the door), Pending -> Pending, Locked / None -> Off for this visit (one [storage] line).
        return Prep::Off;
    }

    void BeginSaleScan(RE::FormID /*a_follower*/) {
        // TODO(author): clear his want list and stamp it (the sell loop is the ONE judge of "would be sold today").
    }

    std::int32_t HoldFromSale(RE::Actor* /*a_follower*/, const FollowerState& /*a_state*/, RE::TESBoundObject* /*a_obj*/,
                              RE::InventoryEntryData* /*a_entry*/, std::int32_t /*a_value*/) {
        // TODO(author): build StoreInput (bucket, class sets from Classes::Get / GetUnlocked / GetMastered / parentMask,
        // rank vs his other carried pieces of the bucket and his stored one, Pricing::Resolve(handle, {}, 0)), call
        // DecideStore, log "[storage] ... HOLD / sell" (rate-limited like the [sell] diag), record a Store on the want list.
        return 0;
    }

    bool Begin(RE::Actor* /*a_follower*/, RE::FormID /*a_chest*/, const std::string& /*a_followerName*/, Clock::time_point /*a_now*/) {
        // TODO(author): create the work record from the want list re-checked against his LIVE inventory (still carried,
        // not worn, not IsStockGear / IsPlayerPick / PlayerGiven / Lotd::HoldFromSale / quest), walk to the chest
        // (APMFBridge::RepointErrandTravel || ClaimErrandTravel, the temple's shrine walk in ErrandRest.cpp StepTemple).
        return false;
    }

    StepOut Step(RE::Actor* /*a_follower*/, Clock::time_point /*a_now*/) {
        // TODO(author): Walk -> Give (APMFBridge::PlayErrandIdle IdleGive; presentation only, a refusal is logged and the
        // move still runs) -> Move (Papyrus::DispatchMoveItem follower -> chest, one call per piece) -> Confirm (his
        // count of the base fell: entry recorded, the superseded entry of the bucket released) -> Done. A piece that
        // did not leave within the confirm window: WARN, no entry, the stop still ends Done when any piece landed.
        StepOut out;
        out.res = Result::Failed;
        out.why = "storage is a template (not implemented)";
        return out;
    }

    void End(RE::FormID /*a_follower*/, const char* /*a_why*/) {
        // TODO(author): drop the work record; APMFBridge::ReleaseErrandIdle. Idempotent.
    }

    bool Working(RE::FormID /*a_follower*/) { return false; }   // TODO(author)

    std::string Describe(RE::FormID /*a_follower*/) { return {}; }   // TODO(author): "Storing gear at home"

    double SimWorkDays() {
        // TODO(author): Config::g_errandTradeRealSec x timescale / 86400 (the shop stop's cost; ErrandSim.cpp WorkDays). No new knob.
        return 0.0;
    }

    std::string SimFinish(RE::FormID /*a_follower*/, const std::string& /*a_followerName*/) {
        // TODO(author): the want list AS PLANNED (an unloaded inventory is never read): one VM move per piece, the entry
        // recorded with Item::kUnverified. RetrieveTick's first loaded pass confirms it (he no longer carries the copy)
        // or drops the entry loudly.
        return "storage is a template (not implemented)";
    }

    void RetrieveTick(RE::Actor* /*a_follower*/, const FollowerState& /*a_state*/, Clock::time_point /*a_now*/) {
        // TODO(author), design section 4. Cadence ~5 s per follower, out of combat, loaded, no menu pause. First settle
        // kUnverified entries. Then per stored piece build RetrieveInput and call DecideRetrieve; on true dispatch
        // Papyrus::DispatchMoveItem chest -> follower, and confirm on a later pass (his count rose -> entry dropped,
        // "[storage] ... RETRIEVED"; not within the window -> WARN + backoff; the third miss drops the entry, loud).
    }

    bool IsStored(RE::FormID a_follower, RE::FormID a_base) {
        std::scoped_lock lk(g_mx);
        const auto it = g_stored.find(a_follower);
        if (it == g_stored.end()) return false;
        for (const auto& i : it->second)
            if (i.base == a_base) return true;
        return false;
    }

    std::vector<Item> StoredOf(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_stored.find(a_follower);
        return it != g_stored.end() ? it->second : std::vector<Item>{};
    }

    std::vector<Home> Homes() {
        std::scoped_lock lk(g_mx);
        return g_homes;
    }

    void Forget(RE::FormID /*a_follower*/) {
        // TODO(author): erase his entries under g_mx; log "[storage] {:08X}: N stored piece(s) released (roster removal)".
    }

    // ── the proposed 'STOR' record (NOT WIRED) ──────────────────────────────────────────────────────────────────
    void CoSave(SKSE::SerializationInterface* /*a_intfc*/) {
        // TODO(author), design section 5. Written ONLY when a home or an entry exists. Snapshot under g_mx, write from the copy.
    }

    void CoLoad(SKSE::SerializationInterface* /*a_intfc*/, std::uint32_t /*a_version*/) {
        // TODO(author), design section 5. v1 is the only version. A short read / implausible count aborts THIS record only.
    }

    void ClearRecord() {
        std::scoped_lock lk(g_mx);
        g_homes.clear();
        g_stored.clear();
    }

    void ClearTransientState() {
        // TODO(author): want lists, per-trip plans, work records, retrieval cadences and confirm windows.
    }
}
