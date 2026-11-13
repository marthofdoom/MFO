#pragma once
#include "PCH.h"
#include "State.h"   // FollowerState

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// logistics/Storage.h -- OFF-CLASS STORAGE (ClickUp 86e3t212u). TEMPLATE ONLY (2026-11-13): every function below is a
// compiling SKELETON with a TODO body. NOTHING here is called from a live path, no co-save record is written or read,
// and no INI key exists yet. Design, layout, seats and the ordered plan: Docs/research/2026-11/offclass-storage-design.md.
//
// WHAT IT WILL BE. marth 2026-10-18: "a follower keep a tracked set expensive off class items that they store in the
// player home, with no player home, they dont store." Rulings 2026-11-12: kept = the economy says its use value later
// beats its sale value now; only equipment he PLANS TO USE; an owned, non-respawning container in a cell the player
// owns, the same chest every time; storing is a STOP on the town errand itinerary; retrieval needs no trip; the tracked
// set is saved; a stored piece is never sold.
//
// THREADING (design section 6). Every entry point runs on the AddTask job worker (the service tick, the errand planner,
// the pump's sweep) EXCEPT: LearnHomeOnMain (MAIN: it walks a loaded cell's references), CoSave / CoLoad (the SKSE save /
// load callbacks, pump paused / stopped) and ClearRecord / ClearTransientState (revert, pump drained). The table sits
// under ONE leaf mutex that is never held across an engine, Harbinger, Papyrus or WriteRecordData call (the PGIV rule).
// Item moves go through the Papyrus VM (ObjectReference.RemoveItem with akOtherContainer): the house rule is "no native
// inventory mutation off screen" (Papyrus.h DispatchRemoveItem), and the chest is usually unloaded.
namespace MFO::Logistics::Storage {

    using Clock = std::chrono::steady_clock;

    // ── the saved data (the proposed 'STOR' v1 record; design section 5 has the byte layout) ────────────────────────
    // A chosen home chest. Identities only (FormIDs, never handles). cell / town are stored with it because an unloaded
    // ref's cell and location chain are not a read this module wants to depend on.
    struct Home {
        RE::FormID chest = 0;   // the container REFR (persistent, player-owned cell, base CONT without kRespawn)
        RE::FormID cell  = 0;   // its parent CELL
        RE::FormID town  = 0;   // LocationTypes::TownAnchor of its location chain (0 = none: retrieval works, storing never plans)
    };
    // One stored piece. ONE entry per (follower, bucket): a better piece of the same bucket RELEASES the older one to the
    // chest's owner (its entry is dropped, the item stays in the chest; it is never sold).
    struct Item {
        static constexpr std::uint8_t kUnverified = 0x01;   // moved off screen (VM dispatch): confirmed on his next loaded tick
        static constexpr std::uint8_t kAllFlags   = 0x01;
        RE::FormID    chest = 0;
        RE::FormID    base  = 0;    // TESObjectWEAP, or a shield TESObjectARMO
        std::uint16_t count = 1;    // 1 today (one piece per bucket); the field leaves room
        std::uint8_t  flags = 0;
    };

    // ── the PURE decision (no engine object: the self-test and the errand planner both call it) ─────────────────────
    // Buckets = the keep set's (SwapUp.cpp ComputeKeepSet bucketOf) plus the shield.
    enum class Bucket : std::uint8_t { None, OneHand, TwoHand, Bow, Crossbow, Staff, Shield };
    enum class Verdict : std::uint8_t {
        Sell,            // today's behaviour: the sale path takes it
        Store,           // hold it from sale and store it at the home stop
    };
    struct StoreInput {
        Bucket        bucket = Bucket::None;
        std::uint16_t classBit = 0;        // Classes::Weapon bit of the piece (0 = none)
        bool          classesOn = false;   // Classes::Active() and a class equipped
        std::uint16_t currentSet = 0;      // his equipped class's weapon set (0 = unrestricted / classes off)
        std::uint16_t plannedSets = 0;     // union of the restricted sets of the classes he can take or that are next on his tree
        int           perkRanks = 0;       // classes OFF: his owned perk ranks in the piece's skill tree (OPEN QUESTION Q-S-1)
        float         rank = 0.0f;         // the bucket's rank (WeaponScore / attack damage / gold value / armor rating)
        float         bestOtherRank = -1.0f;   // best rank among every OTHER carried piece of the bucket and his stored piece (-1 = none)
        bool          tieLoses = false;    // an equal-ranked other piece wins the tie (value, then the lower FormID)
        std::int32_t  value = 0;           // the instance's base value (InventoryEntryData::GetValue)
        float         buyMult = 1.0f;      // Pricing::Quote::buy  (>= 1)
        float         sellMult = 1.0f;     // Pricing::Quote::sell (<= 1)
        std::int32_t  minMargin = 0;       // Config::g_errandMinSellGold: what makes a stop worth walking
        bool          homeReady = false;   // Prepare said a home stop can be planned on this trip
    };
    struct StoreOutput {
        Verdict      verdict = Verdict::Sell;
        std::int32_t useValue = 0, saleValue = 0;   // Pricing::Apply(value, buy / sell)
        const char*  why = "";                       // one word group for the [storage] line
    };
    // TODO(author): design section 2 + 3. No side effect, no engine read.
    StoreOutput DecideStore(const StoreInput& a_in);

    struct RetrieveInput {
        Bucket        bucket = Bucket::None;
        std::uint16_t classBit = 0;
        std::uint16_t currentSet = 0;        // 0 = unrestricted / classes off
        bool          roleKeeps = false;     // his weapon roles keep this bucket now (ComputeWeaponRoles; shield: off hand is not a weapon)
        float         rank = 0.0f;
        float         bestCarriedRank = -1.0f;   // best carried piece of the bucket (-1 = none)
        bool          classChanged = false;  // his equipped class differs from the last evaluation's (the log's reason only)
    };
    // TODO(author): design section 4. True = bring it back now.
    bool DecideRetrieve(const RetrieveInput& a_in, const char** a_whyOut = nullptr);

    // ── live seats (ALL UNCALLED today; the design's plan wires them one by one) ────────────────────────────────────
    // INI switch + Harbinger errand support. TODO(author): Config::g_offClassStorage (bOffClassStorage, default ON).
    bool Enabled();

    // MAIN thread. The player stands in a cell he owns: pick (or confirm) that cell's chest. Design section 1.
    void LearnHomeOnMain();
    // Worker, cheap: ask for LearnHomeOnMain when the player's cell changed (posts at most once per cell).
    void NoteWorkerTick();

    // Errand planner (worker, TryStart, BEFORE EconomyErrandPlan): can THIS trip carry a home stop for him?
    enum class Prep : std::uint8_t { Off, Ready, Pending };
    Prep Prepare(RE::Actor* a_follower, RE::FormID a_playerTown);

    // Economy.cpp sell loop (worker): how many copies of this sell candidate are held back for the home stop (0 = none,
    // today's sale). a_value = the instance's base value. Records the piece on his want list. Call BeginSaleScan first.
    void BeginSaleScan(RE::FormID a_follower);
    std::int32_t HoldFromSale(RE::Actor* a_follower, const FollowerState& a_state, RE::TESBoundObject* a_obj,
                              RE::InventoryEntryData* a_entry, std::int32_t a_value);

    // The physical stop (worker; the Smith stop's shape, logistics/ErrandSmith.cpp). The planner half (Augment) takes the
    // itinerary types of Errand_internal.h and is therefore declared THERE when it is wired (design section 8, step 6).
    enum class Result : std::uint8_t { Continue, Done, Failed };
    struct StepOut { Result res = Result::Continue; std::string why; };
    bool Begin(RE::Actor* a_follower, RE::FormID a_chest, const std::string& a_followerName, Clock::time_point a_now);
    StepOut Step(RE::Actor* a_follower, Clock::time_point a_now);
    void End(RE::FormID a_follower, const char* a_why);
    bool Working(RE::FormID a_follower);
    std::string Describe(RE::FormID a_follower);   // "Storing gear at home" ("" = no work). Any thread.

    // Off screen (worker): the stage's game-day cost and its end (VM dispatch, entries flagged kUnverified).
    double SimWorkDays();
    std::string SimFinish(RE::FormID a_follower, const std::string& a_followerName);

    // Retrieval, from anywhere (worker, the idle branch of the service tick, on its own cadence).
    void RetrieveTick(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now);

    // Queries. Any thread (they copy under the leaf mutex).
    bool IsStored(RE::FormID a_follower, RE::FormID a_base);
    std::vector<Item> StoredOf(RE::FormID a_follower);
    std::vector<Home> Homes();

    // Roster removal (Followers::DrainRosterRemovals, beside PlayerGiven::Forget): his entries go, the items stay in the chest.
    void Forget(RE::FormID a_follower);

    // ── the proposed 'STOR' co-save record (NOT WIRED: Serialization.h / Serialization.cpp do not know it yet) ────────
    void CoSave(SKSE::SerializationInterface* a_intfc);                          // 'STOR' v1
    void CoLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version); // version-checked by the caller
    void ClearRecord();            // ResetAllState after StopPump, and LoadCallback start (the record is SAVED)
    void ClearTransientState();    // want lists, plans, work records, cadences (session-only)
}
