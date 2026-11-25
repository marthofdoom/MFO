#pragma once
// iaus_sim/Move.h -- SIM V4, movement package S2 (Docs/research/2026-11/movement-plan.md): the switches, the numbers and the per-fight state of the
// shared movement pass in the sim's world. HOST-ONLY. The decision is the game's own (native/iaus MovePass / Spots): MoveWorld.h fills its picture from
// the fight, answers its rays with one window of latency and carries out what it assigned. Off (the default) nothing here runs: MoveBox is null.
//
// TO ADD OR CHANGE
//   a number of the mover's model ........ MoveTuning (one table, PLACEHOLDERS until the field exerciser G0b reports)
//   a fault variant of a scenario ........ kMoveFaults (one table), applied only in MoveWorld.h
#include "iaus/MoveDump.h"
#include "iaus/Generate.h"

#include <array>
#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

namespace Sim {

    namespace MV = MFO::Iaus::Move;
    namespace MP = MFO::Iaus::MovePass;
    namespace TR = MFO::Iaus::Trust;

    // --position-road: what carries an assignment out. Point = the ABI today (ch.24 point + hold: he fights the whole way, stays inside R, may drift in it).
    // Pinned = the H1 wish (he stands still on it and the pin kinds may be offered, with t.spotPinKinds=1). Off = no pass at all.
    enum class Road : std::uint8_t { Off = 0, Point, Pinned };
    inline Road g_road = Road::Off;
    inline bool g_spotMeter = false;              // count cover / exposure (one look per threat each 0.5 s). Any --position-road sets it, `off` too, so both sides can be read
    inline std::uint32_t g_spotsReplace = 0;      // --spots-replace <kind,...>: the families these kinds carry file no drive of their own, the spot claim does

    // THE MOVER'S MODEL. Every row a PLACEHOLDER (plan S2, replaced once G0b's field data exists).
    struct MoveTuning {
        float radiusU = 90.0f;        // the claim's radius R (param.fval). Equal to t.spotArriveU, so "inside R" and "arrived" are one thing
        float pinRadiusU = 40.0f;     // ... on the pinned road
        float insideU = 45.0f;        // he walks on until this near the point, then holds (the walk does not stop on R's rim and step out again)
        float speedX = 1.0f;          // walking speed on the claim x his run speed (arrival time against path length)
        float driftU = 40.0f;         // point road: how far from the point he shifts while he holds ("moving tactically inside it")
        float driftSec = 3.0f;        // ... and how often
        float driftSpeedX = 0.4f;
        float dropShare = 0.0f;       // the share of filed claims the engine drops for no reason the navmesh shows
        bool  fightsOnTheWay = true;  // a foe inside his swing reach is fought while the claim stands
    };
    inline MoveTuning g_moveT;

    // THE FAULT VARIANTS: one mechanism of the pass broken from outside (the pure code has no hook), so a scenario's rule is shown to test it.
    struct MoveFaultRow {
        const char* name; std::uint32_t kinds = 0 /* never offered */; bool terrainBlind = false /* the pass's rays do not see the ground */; bool blind = false /* ... nor the walls */;
        bool reachBlind = false /* who cannot be walked to is not known */; bool noPortals = false /* no place stands in a doorway */; bool noExits = false /* a dead end counts as a way on */;
        bool noRays = false /* no ray is ever answered */; bool stayHolds = false /* "stay" is carried out as a claim on his own place */; bool meleeHands = false /* the pass is told every attack is a blade's */;
        bool noTrust = false /* no mark is ever kept (S3) */; bool hairTrust = false /* one lived sign distrusts: the 11-17 GATED mistake */; bool noMemory = false /* a blocked leg does not remember its spot */; bool noWalked = false /* no walk clears a mark (S3 review R6) */; bool anyLeg = false /* any goal-less leg that ends ends the claim (S2 review F6) */;
    };
    inline constexpr MoveFaultRow kMoveFaults[] = {
        { .name = "none" },
        { .name = "no-heal", .kinds = MV::KindBit(MFO::Iaus::SpotKind::HealReach) },
        { .name = "terrain", .terrainBlind = true },
        { .name = "blind", .terrainBlind = true, .blind = true },
        { .name = "reach", .reachBlind = true },
        { .name = "portals", .noPortals = true },
        { .name = "exits", .noExits = true },
        { .name = "rays", .noRays = true },
        { .name = "stay-holds", .stayHolds = true },
        { .name = "melee-hands", .meleeHands = true },
        { .name = "no-trust", .noTrust = true },
        { .name = "hair-trust", .hairTrust = true },
        { .name = "no-memory", .noMemory = true },
        { .name = "no-walked", .noWalked = true },
        { .name = "any-leg", .anyLeg = true },
    };
    inline const MoveFaultRow* g_moveFault = &kMoveFaults[0];
    [[nodiscard]] inline const MoveFaultRow* MoveFaultByName(std::string_view n) { for (const MoveFaultRow& f : kMoveFaults) if (n == f.name) return &f; return nullptr; }

    // What one follower's claim is doing (by Fight::ctl index).
    struct MoveClaim {
        bool on = false, arrived = false, dropDue = false;
        MV::SpotKey key = 0; EQ::Vec3 p; std::int32_t tri = -1; std::uint32_t kinds = 0; MFO::Iaus::RankedAction act;
        double since = 0.0; float dx = 0.0f, dy = 0.0f; double driftAt = 0.0;
        std::array<MFO::Iaus::RankedAction, 3> top{}; std::uint8_t nTop = 0; double topAt = -1e9;   // his last ranking (TopRanked at his own lap)
        float burstSeen = 0.0f;
        // what trust hears of the leg (S3): the planned walk, his own, which of its points he came by. len 0 = nothing to compare (his own movement broke into it)
        bool walks = false;                 // the step in hand is this claim's (a leg that ends in another's step is not this claim's leg)
        std::uint32_t leg = 0; float len = 0.0f, walked = 0.0f; EQ::Vec3 from; std::vector<EQ::Vec3> via; std::uint32_t passed = 0; double walkAt = -1e9;
    };
    struct MoveState {
        MV::PartyPicture pic; std::vector<MoveClaim> claim;
        std::vector<MV::RayAsk> asks; std::size_t asked = 0; std::uint32_t legs = 0; double winStart = 0.0, winEnd = 0.0; bool open = false;
        std::vector<MFO::Iaus::Trust::Line> trustLog; std::vector<EQ::Vec3> lastAt;   // S3: every [env-trust] line of the fight, and where each body was a step ago
        std::vector<MV::Assignment> last;   // what the last window assigned (diagnostics, the scenario tests)
    };
    // A fight is a plain value: a copy copies the state too. Null while the road is off.
    struct MoveBox {
        std::unique_ptr<MoveState> s;
        MoveBox() = default; MoveBox(MoveBox&&) = default; MoveBox& operator=(MoveBox&&) = default;
        MoveBox(const MoveBox& o) : s(o.s ? std::make_unique<MoveState>(*o.s) : nullptr) {}
        MoveBox& operator=(const MoveBox& o) { if (this != &o) s = o.s ? std::make_unique<MoveState>(*o.s) : nullptr; return *this; }
        explicit operator bool() const { return static_cast<bool>(s); }
        MoveState* operator->() const { return s.get(); }
    };

    // COST, printed as the MOVECOST line. Host time, in no metric and no compare.
    inline std::atomic<long long> g_moveNs{ 0 }, g_moveWorstNs{ 0 }, g_moveWindows{ 0 }, g_moveRays{ 0 }, g_moveFights{ 0 };

}
