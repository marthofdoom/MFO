#pragma once
#include "PCH.h"
#include <chrono>
#include <functional>
#include <string>
#include <vector>
#include "State.h"   // FollowerState

// logistics/Errand.h -- TOWN ERRANDS, round R1a (ClickUp 86e3t1mx7; design: _evidence/2026-10-18-designs/
// TOWN-DESIGN.md). With Harbinger (ch.19 travel + ch.12 idle v2 + GetIdleState, ABI >= 24) present, the
// follower no longer trades INSTANTLY with any vendor near him (the old EconomyProbe proximity scan): when the
// PLAYER is in a City / Town / Settlement and the follower has a reason, he WALKS to an open shop's vendor
// (ch.19), plays talk / give / take idles AT him (ch.1 hold + ch.12 v2, the Deposit pattern), trades through
// the unchanged TradeBridge::VendorTrade, and rejoins (every claim released; his follow package walks him
// back). Harbinger absent (or bTownErrands off) = the legacy proximity trade, unchanged.
//
// R1a SCOPE: the vendor is a LOADED actor (exterior stalls, the forge, or the player inside the shop). The
// catalog reads each candidate's vendor FACTION (hours, fence flag, conditions, VEND list, chest) but takes the
// PLACE from the loaded vendor actor, not from the faction's PLVD location (its in-memory union is unverified;
// see the seams below). No off-screen simulation: a follower who unloads ends the errand.
//
// SEAMS for R1b / R2 (do not build here): the state machine's Phase enum + Errand record (Errand.cpp) are
// where OUT_SIM / TRADE_SIM / RET_SIM / RECALL and the entryDoor attach; Sweep() is the owner-independent
// tick the simulation clock rides; CatalogEntry::place is where the faction's PLVD cell / ref goes once its
// layout is verified; a PLAN-time stored TradeOrder replaces the fresh EconomyProbe(kExecute) call; the
// ERND co-save record serialises the Errand record.
//
// THREADING: every entry point runs on the AddTask job worker (the per-follower service tick, the pump's
// Sweep) except ClearTransientState (revert, pump drained). One mutex guards the table; it is never held
// across a Harbinger or TradeBridge call.
namespace MFO::Logistics::Errand {

    using Clock = std::chrono::steady_clock;

    // The idle branch of ServiceFollower, in place of EconomyProbe. TRUE = errands own the economy for this
    // follower (the caller must NOT run the legacy proximity trade); false = legacy applies (bTownErrands
    // off, or Harbinger absent / too old).
    bool TryStart(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now);
    // Top of the logistics tick: drive this follower's errand. TRUE = the errand owns this tick.
    bool Tick(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now,
              const std::function<bool()>& a_healWants = {});
    // The owner-independent backstop (the pump, every lap): ends an errand whose follower is dead, unloaded,
    // in combat, dismissed, no longer serviced, or whose player left the town; enforces the caps.
    void Sweep();
    // End one follower's errand now (idempotent).
    void End(RE::FormID a_follower, const char* a_why);
    // Is this follower on an errand? (board / logging)
    bool Active(RE::FormID a_follower);
    // R3: a short status of this follower's errand for the Board ("Walking to X", "Trading with X", "Off-screen: trading at X",
    // "Heading back"), physical or simulated. EMPTY = no errand. Any thread: it copies under the table mutexes (physical, then
    // simulated, never nested) and reads no engine object.
    std::string Describe(RE::FormID a_follower);
    // Revert / load (pump drained): drop every errand and its claims (and any restored entry not yet adopted).
    void ClearTransientState();

    // ── R2: the 'ERND' co-save record (layout + opcodes in Serialization.h / Serialization.cpp) ─────────────
    // One errand as the save keeps it: identities (FormIDs, never handles), the stage, the trade flags and the
    // game-day clock stamps. The TRADE ORDER is NOT saved (MFO never rebuilds it from an unloaded inventory):
    // a restored errand whose trade had not run is either re-planned on screen (he is loaded at the shop) or
    // DROPS the trade (marth 2026-10-19: "save before the off-screen trade ran -> drop the trade").
    enum class SavedStage : std::uint8_t {
        PhysWalking, PhysAtVendor, PhysTrading, PhysTaking,   // the PHYSICAL errand (Errand.cpp)
        SimOut, SimTrade, SimReturn, SimRejoin                // the OFF-SCREEN simulation (ErrandSim.cpp)
    };
    enum class SavedDest : std::uint8_t { VendorActor, Ref, Cell };
    // R5 (ERND v2): one STOP of a multi-stop trip. Kind and state are persisted as string opcodes (#10, tables in
    // Serialization.cpp): a later round APPENDS a kind (fence, spouse, smith, temple, inn, home), never renumbers one.
    inline constexpr std::size_t kMaxTripStopsOnWire = 8;   // #11: the stop-count bound of an ERND v2 entry (writer and reader)
    enum class SavedStopKind : std::uint8_t { Shop, Temple, Inn };   // R7 appended "temple" and "inn" (opcode strings; the layout is unchanged)
    enum class SavedStopState : std::uint8_t { Pending, Active, Done, Skipped };
    struct SavedStop {
        SavedStopKind  kind = SavedStopKind::Shop;
        RE::FormID     faction = 0, chest = 0, vendorActor = 0;
        SavedDest      destKind = SavedDest::Cell;
        RE::FormID     destForm = 0;
        float          destRad = 0.0f;
        SavedStopState state = SavedStopState::Pending;
        double         tArrive = 0.0, tWorkEnd = 0.0;   // GAME DAYS of the simulation (0 = not reached yet / physical)
    };
    struct Saved {
        static constexpr std::uint8_t kArrived    = 0x01;   // he had reached the shop / vendor
        static constexpr std::uint8_t kDispatched = 0x02;   // a trade was in flight at the save (it is NOT replayed)
        static constexpr std::uint8_t kTradeDone  = 0x04;   // the trade reported: only the walk back is left
        static constexpr std::uint8_t kHadOrder   = 0x08;   // a non-empty order existed (a physical resume may re-plan it)
        static constexpr std::uint8_t kAbandoned  = 0x10;   // the trade was already given up / dropped
        static constexpr std::uint8_t kAllFlags   = 0x1F;
        RE::FormID    follower = 0;
        SavedStage    stage = SavedStage::SimOut;
        std::uint8_t  flags = 0;
        RE::FormID    town = 0, faction = 0, chest = 0, vendorActor = 0;
        SavedDest     destKind = SavedDest::Cell;
        RE::FormID    destForm = 0;
        float         destRad = 0.0f;
        RE::FormID    entryDoor = 0, lastCell = 0;
        RE::NiPoint3  lastPos{};
        float         startDays = 0.0f;
        double        tEnter = 0.0, tArrive = 0.0, tTradeEnd = 0.0, tReturn = 0.0;   // GAME DAYS (0 for a physical stage)
        // R5 (ERND v2): the trip's itinerary. The fields above (faction ... destRad, tArrive, tTradeEnd) describe the CURRENT
        // stop, stops[cur]; the writers keep them equal. A v1 entry loads as ONE stop (cur 0, Active) built from them.
        std::vector<SavedStop> stops;
        std::uint8_t           cur = 0;
    };
    // SaveCallback (main thread, pump paused): every live errand, physical and simulated, plus any restored entry
    // the pump has not adopted yet (a save right after a load must not lose it).
    std::vector<Saved> SnapshotForSave();
    // LoadCallback (main thread, pump stopped, after the revert): stage the resolved entries. The pump ADOPTS them
    // on its first lap with the player in the world (Sim::Sweep): dead / gone / dismissed -> dropped, loaded at the
    // shop -> the physical errand resumes, unloaded -> the simulation continues on the game clock.
    void RestoreFromSave(std::vector<Saved>&& a_entries);
    // LoadCallback start (belt-and-braces beside the revert): forget staged entries.
    void ClearRestorePending();
}
