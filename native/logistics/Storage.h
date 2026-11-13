#pragma once
#include "PCH.h"
#include "State.h"            // FollowerState
#include "classes/Classes.h"  // Classes::Mask / Id (the pure plan reads the class table)

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace MFO { class CoSaveBuffer; }   // Serialization.h (the round-trip self-test's byte buffer)

// logistics/Storage.h -- OFF-CLASS STORAGE (ClickUp 86e3t212u). Design: Docs/research/2026-11/offclass-storage-design.md.
//
// WHAT IT IS. marth 2026-10-18: "a follower keep a tracked set expensive off class items that they store in the player home".
// Rulings (verbatim in the design doc's section 0a; they override the first design):
//   * 2026-11-12, classes OFF: "No storing" (he sells as today);
//   * 2026-11-12, what: "It only stores for previously mastered, or future in line. But Armor too." -- a piece is PLANNED only
//     for a class he has mastered or one ahead of his equipped class on his own tree line (same T1 root); weapons, shields AND
//     armor;
//   * 2026-11-12, access: "As I said they can access it from anywhere. But only for this purpose." -- no trip, no errand stop:
//     the move to and from the chest is a Papyrus VM RemoveItem wherever he is (Papyrus::DispatchMoveItem);
//   * 2026-11-13 (Q-S-13), where: "Give the followers thier own mfo storage they can access." -- ONE MFO-owned persistent chest
//     generated in MFO.esp (Forms::g_storageChest, 0x919, in an MFO holding cell), shared by every follower; each follower's
//     pieces are tracked per follower in the 'STOR' record. No home search, no interim chest, no migration. Chest missing (old
//     ESP) = no storing;
//   * the economy decides what is worth keeping (use value later >= sale value now + iErrandMinSellGold, a placeholder);
//   * retrieval: on a class change, and when a stored piece is better than what he carries (= the sale path would keep it).
//
// THE JUDGE OF "WOULD BE SOLD" IS THE SALE PATH ITSELF (logistics/Economy.cpp, the sell-candidate loop): HoldFromSale is asked
// only for an unworn weapon / armor row that cleared every protection above it. The judge of "would be KEPT" on the way back is
// the same sale path too: ComputeKeepSet(..., a_extra) + SaleKeepsUnworn (Economy.cpp), never a copy of their rules.
//
// THREADING. Every entry point runs on the AddTask job worker (the service tick and the sale loop it drives) EXCEPT CoSave /
// CoLoad (the SKSE save / load callbacks, pump paused / stopped) and ClearRecord / ClearTransientState (revert, pump drained).
// The tables sit under ONE leaf mutex that is never held across an engine, Papyrus or WriteRecordData call. The chest's
// inventory is never read: every move is confirmed from the FOLLOWER's own count (the chest is shared by every follower).
namespace MFO::Logistics::Storage {

    using Clock = std::chrono::steady_clock;

    // ── the saved data (the 'STOR' v1 record; Serialization.h has the byte layout) ─────────────────────────────────────
    // One stored piece of one follower, in the MFO chest. ONE entry per (follower, Bucket): a better piece of the same bucket
    // RELEASES the older one (its entry is dropped, the item stays in the chest; it is never sold). Never two entries of one base.
    struct Item {
        static constexpr std::uint8_t kUnverified = 0x01;   // a store was in flight at the save: settled in a fresh window after the load
        static constexpr std::uint8_t kRetrieving = 0x02;   // a retrieval was in flight at the save: settled the same way
        static constexpr std::uint8_t kAllFlags   = 0x03;
        RE::FormID    base  = 0;    // TESObjectWEAP or TESObjectARMO
        std::uint16_t count = 1;    // 1 (one piece per bucket); the field leaves room
        std::uint8_t  flags = 0;
    };
    struct Table {
        std::unordered_map<RE::FormID, std::vector<Item>> stored;   // follower -> his pieces
    };

    // ── the PURE decision (no engine object: the self-test calls it table-driven) ────────────────────────────────────────
    enum class Bucket : std::uint8_t {
        None, Dagger, OneHand, TwoHand, Bow, Crossbow, Staff, Shield,
        HeadRated, BodyRated, HandsRated, FeetRated,
        HeadCloth, BodyCloth, HandsCloth, FeetCloth,
        kCount
    };
    const char* BucketName(Bucket a_b);
    // Armor kinds (a class wears one of them): rated armor, or clothing (the mage line with bMageWearRobes on).
    namespace Kind {
        inline constexpr std::uint8_t kRated    = 1 << 0;
        inline constexpr std::uint8_t kClothing = 1 << 1;
    }
    // The armor kind class a_class wears. a_robes = bMageWearRobes on and dolls mode off.
    std::uint8_t KindOf(Classes::Id a_class, bool a_robes);

    // What his classes plan: the union over PLANNED classes = (mastered | the classes ahead of a_equipped with the same T1
    // root, transitively through parentMask), minus a_equipped. Weapon bits skip unrestricted sets (T1, Dragonborn: they fit
    // everything and would plan every item); armor kinds skip a class with no T1 root (Dragonborn).
    struct Plan {
        std::uint16_t weapons = 0;
        std::uint8_t  kinds   = 0;
        Classes::Mask classes = 0;   // the planned classes (for the log)
    };
    Plan PlannedFor(std::uint8_t a_equipped, Classes::Mask a_mastered, bool a_robes);

    enum class Verdict : std::uint8_t { Sell, Store };
    struct StoreInput {
        Bucket        bucket = Bucket::None;
        std::uint16_t classBit = 0;        // Classes::Weapon bit of a weapon / shield (0 for armor)
        std::uint8_t  armorKind = 0;       // Kind:: of an armor piece (0 for a weapon / shield)
        bool          classesOn = false;   // Classes::Active() and a class equipped
        std::uint16_t currentSet = 0;      // his equipped class's weapon set (0 = unrestricted)
        std::uint8_t  currentKind = 0;     // the armor kind his equipped class wears
        Plan          plan;                // PlannedFor(...)
        bool          storageReady = false;   // the MFO chest resolved (the ESP is current)
        bool          singleCopy = true;   // he carries exactly one copy (a store is confirmed by his count)
        float         rank = 0.0f;         // the bucket's rank (StoreRank in Storage.cpp)
        float         bestOtherRank = -1.0f;   // best rank among every OTHER carried piece of the bucket and his stored one (-1 = none)
        bool          tieLoses = false;    // an equal-ranked other piece wins the tie (value, then the lower FormID)
        std::int32_t  value = 0;           // the instance's base value (InventoryEntryData::GetValue)
        float         buyMult = 1.0f;      // Pricing::Quote::buy  (>= 1)
        float         sellMult = 1.0f;     // Pricing::Quote::sell (<= 1)
        std::int32_t  minMargin = 0;       // Config::g_errandMinSellGold (PLACEHOLDER for the sim logistics world, Q-S-3)
    };
    struct StoreOutput {
        Verdict      verdict = Verdict::Sell;
        std::int32_t useValue = 0, saleValue = 0;   // Pricing::Apply(value, buy / sell)
        const char*  why = "";                       // one word group for the [storage] line
    };
    StoreOutput DecideStore(const StoreInput& a_in);
    // The class half of DecideStore alone (cheap, no engine read): false = it sells whatever the ranks and the prices say.
    bool PlannedOffClass(const StoreInput& a_in, const char** a_whyOut = nullptr);

    struct RetrieveInput {
        Bucket        bucket = Bucket::None;
        std::uint16_t classBit = 0;
        std::uint8_t  armorKind = 0;
        std::uint16_t currentSet = 0;        // 0 = unrestricted / classes off
        std::uint8_t  currentKind = 0;       // the armor kind he wears now
        bool          saleKeeps = false;     // the sale path would KEEP it were he carrying it (ComputeKeepSet a_extra + SaleKeepsUnworn)
        bool          classChanged = false;  // his equipped class differs from the last evaluation's (the log's reason only)
    };
    bool DecideRetrieve(const RetrieveInput& a_in, const char** a_whyOut = nullptr);

    // ── live seats (WORKER) ────────────────────────────────────────────────────────────────────────────────────────────
    // bOffClassStorage AND the MFO chest resolved. (Classes OFF is decided per follower inside: no class equipped = no storing.)
    bool Enabled();

    // Economy.cpp sell loop: how many copies of this sell candidate are held back from the sale (0 = sell as today, 1 = it
    // is going to storage). Records the piece on his want list; Tick moves it.
    std::int32_t HoldFromSale(RE::Actor* a_follower, const FollowerState& a_state, RE::TESBoundObject* a_obj,
                              RE::InventoryEntryData* a_entry, std::int32_t a_count, std::int32_t a_value);

    // The service tick's idle branch (logistics/Service.cpp), out of combat, loaded: settle what a load restored mid-move,
    // confirm in-flight moves, dispatch wanted stores, retrieve one piece.
    void Tick(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now);

    // Queries. Any thread (they copy under the leaf mutex).
    bool IsStored(RE::FormID a_follower, RE::FormID a_base);
    std::vector<Item> StoredOf(RE::FormID a_follower);

    // Roster removal (Followers::DrainRosterRemovals, beside PlayerGiven::Forget): his entries go, the items stay in the chest.
    void Forget(RE::FormID a_follower);

    // ── the 'STOR' co-save record (Serialization.h has the layout) ────────────────────────────────────────────────────
    void CoSave(SKSE::SerializationInterface* a_intfc);                          // 'STOR' v1, written only when non-empty
    void CoLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version); // version-checked by the caller
    void ClearRecord();            // ResetAllState after StopPump, and LoadCallback start (the record is SAVED)
    void ClearTransientState();    // want lists, in-flight moves, cadences (session-only)

    // The codec on a plain Table (the self-test's round trip; CoSave / CoLoad run the same template on the SKSE interface).
    // ReadBody resolves every id (#8) and drops what does not resolve; false = the record was aborted (short read /
    // implausible count) and what was parsed before it is kept.
    struct LoadStats {
        std::uint32_t droppedFollowers = 0, droppedItems = 0, clampedFlags = 0, skippedRuntime = 0;
    };
    void WriteBody(CoSaveBuffer& a_out, const Table& a_table, LoadStats* a_stats = nullptr);
    bool ReadBody(CoSaveBuffer& a_in, std::uint32_t a_version, Table& a_out, LoadStats& a_stats);

#ifdef MFO_DEV_SELFTEST
    // DEV builds only (selftest storage.chest_roundtrip, review F1): arm a REAL round trip of one copy of a_base between the
    // follower and the MFO chest, run by Tick on the worker (him -> chest, then chest -> him, each hop confirmed from his count).
    // Armed / disarmed by the self-test runner's setup (a probe bit), read by the test.
    struct DevRoundTrip { bool armed = false, done = false, ok = false; std::string evidence; };
    void         DevArmRoundTrip(RE::FormID a_follower, RE::FormID a_base);
    void         DevDisarmRoundTrip(RE::FormID a_follower);
    DevRoundTrip DevReadRoundTrip(RE::FormID a_follower);
#endif
}
