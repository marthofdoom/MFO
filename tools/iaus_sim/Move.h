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

    struct Fight;

    namespace MV = MFO::Iaus::Move;
    namespace MP = MFO::Iaus::MovePass;
    namespace TR = MFO::Iaus::Trust;

    // --position-road: what carries an assignment out. Point = the ABI today (ch.24 point + hold: he fights the whole way, stays inside R, may drift in it).
    // Pinned = the H1 wish (he stands still on it and the pin kinds may be offered, with t.spotPinKinds=1). Off = no pass at all.
    // Strict = Harbinger's strict orders (ABI 48): the pass's MoveOrder, carried out by StrictWorld.h, under the residue table.
    enum class Road : std::uint8_t { Off = 0, Point, Pinned, Strict };
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
        float sneakX = 0.55f;         // the sneak gait (World.h Move)
        // the strict order's own model (no field row waits for these)
        float strictMinStopU = 6.0f;  // he is never asked to stop nearer than this
        float strictAimCos = 0.7071f; // travel within 45 degrees of the foe he faces is toward it, past 135 away from it, between across it
        float strictDriftX = 0.3f, strictDriftSec = 3.0f;   // Residue::standDrift: how fast, how often another place
        float lungeStandoffU = 60.0f; // a lunge ends this far from the body it is aimed at
        float blockAfterSec = 0.5f, blockAgainSec = 1.0f;   // Residue::blockedShare: the first block so long after the order, the next so long after the back-off
    };
    inline MoveTuning g_moveT;

    // THE RESIDUE TABLE (tight-movement-plan.md 11 item 3, hm1-seat-brief.md 15, s5-plan.md 4.1): what a strict order does NOT hold. One row a number, every one a
    // PLACEHOLDER until the dev gambit row named beside it is read (calib_point.py rewrites it then) unless it says where it is from. --residue <name>=<v>, in the
    // world tag when it is not the default. Read by the strict executor only (StrictWorld.h, and MoveWorld.h where an order is published or a leg ends).
    // NOT MODELLED: package areas (Yielded), the Exit Water pick, flight, a mount. The ORDER MODEL'S OWN numbers (no field row) are MoveTuning's.
    // TO ADD A RESIDUE: a member and its kResidue row (lo and hi = the sensitivity arms of s5-plan.md 9), its reader, its spot-test row with the row zeroed.
    struct Residue {
        float selectorLag = 0.25f;       // the Movement Selector's period
        float startDelay = 0.0f;         // the first order's first carry: 0 with the pick seat, up to 1.0 on the area road (areaLag)
        float repointStall = 0.25f;      // one pick of standing when the point changes in flight
        float refileStall = 1.1f;        // a NEW point is a release, a request and an area carry until P-C, and so is the first order after a Free (Free ends the claim). Until it is carried the ENGINE moves him (as under startDelay)
        float bridgeLag = 0.0f;          // from the pass window to the order being live: until then the one before stands
        float arriveError = 1.0f;        // he stops inside U(0, arriveError x arriveR), never past leashR (2.7 x 24 = the accepted short stop)
        float arriveZ = 0.0f;            // > 0: a point whose height is more than this off his never arrives, the leg blocks (Harbinger measures in 3D). 0 = off
        float standDrift = 0.0f;         // what he moves of himself while he stands
        float kiteSpeed = 150.0f;        // u/s while he backs away from the foe he faces (RUN: 60 to 193)
        float strafeSpeed = 150.0f;      // ... while he goes across it
        float walkBackSpeed = 150.0f;    // ... while he is walked back to the place he was carried off
        float shotStop = 0.0f;           // seconds of travel a shot or a cast begun on the way costs
        float lungeOut = 60.0f;          // what one started blow carries him
        float lungeOutPower = 110.0f;    // ... one power attack
        float lungeShare = 0.5f;         // the share of started blows that carry him at all
        float attackInPlaceRate = 0.6f;  // the attack rate of an order with kMove_AttackInPlace. marth: lunges are allowed, so the pass files no such order and this row is not armed
        float blockedShare = 0.05f;      // the share of orders that end Blocked for no reason the navmesh shows
        float blockBackoff = 1.0f;       // he stands this long after a block (MoveTuning kBlockBackoffMs)
        float blockWindow = 10.0f;       // three blocks in it end the order
        float blockTold = 0.0f;          // 0: a block never becomes Unreachable and nobody is told (the game until P-B)
        float displaceU = 0.0f;          // what a stagger pushes him (displaceReturn: the walk back follows from it)
        float displaceDownU = 0.0f;      // ... a fall (the lab's one fall is a paralysis: in a cell without one the row is NOT ARMED, no verdict may be read there)
        float dodgeInLeash = 1.0f;       // 1 = a dodge is cut at the leash (the clamp), 0 = refused
        float fearUncarried = 1.5f;      // seconds of every fear effect his order is not carried: the engine's, with Flee and Hide denied
        float fearActs = 1.0f;           // 0: ... and he begins no attack and no hostile cast in them
        float notCarriedShare = 0.0f;    // the share of points filed that are never carried: the engine moves him as long as that order stands
    };
    inline Residue g_residue;
    struct ResidueRow { const char* name; float Residue::*f; float lo, hi; const char* by; };
    inline constexpr ResidueRow kResidue[] = {
        { "selectorLag", &Residue::selectorLag, 0.25f, 0.25f, "DISASM" },          { "startDelay", &Residue::startDelay, 0.0f, 1.0f, "stand session" },
        { "repointStall", &Residue::repointStall, 0.25f, 0.25f, "DISASM" },        { "refileStall", &Residue::refileStall, 0.25f, 1.1f, "P-C session" },
        { "bridgeLag", &Residue::bridgeLag, 0.0f, 0.27f, "any session" },          { "arriveError", &Residue::arriveError, 1.0f, 64.0f / 24.0f, "strict.goto" },
        { "arriveZ", &Residue::arriveZ, 0.0f, 24.0f, "strict.goto on a slope" },   { "standDrift", &Residue::standDrift, 0.0f, 15.0f, "stand session" },
        { "kiteSpeed", &Residue::kiteSpeed, 60.0f, 193.0f, "H-M2 session" },       { "strafeSpeed", &Residue::strafeSpeed, 150.0f, 370.0f, "H-M2 session" },
        { "walkBackSpeed", &Residue::walkBackSpeed, 60.0f, 193.0f, "stand session" }, { "shotStop", &Residue::shotStop, 0.0f, 1.0f, "H-M2 session" },
        { "lungeOut", &Residue::lungeOut, 0.0f, 120.0f, "stand session" },         { "lungeOutPower", &Residue::lungeOutPower, 0.0f, 200.0f, "stand session" },
        { "lungeShare", &Residue::lungeShare, 0.5f, 1.0f, "stand session" },       { "attackInPlaceRate", &Residue::attackInPlaceRate, 0.6f, 0.6f, "not armed" },
        { "blockedShare", &Residue::blockedShare, 0.0f, 0.22f, "every session" },  { "blockBackoff", &Residue::blockBackoff, 1.0f, 1.0f, "hm1 7.2" },
        { "blockWindow", &Residue::blockWindow, 10.0f, 10.0f, "hm1 7.2" },         { "blockTold", &Residue::blockTold, 0.0f, 1.0f, "P-B" },
        { "displaceU", &Residue::displaceU, 0.0f, 120.0f, "stand session" },       { "displaceDownU", &Residue::displaceDownU, 0.0f, 300.0f, "stand session" },
        { "dodgeInLeash", &Residue::dodgeInLeash, 1.0f, 1.0f, "DISASM" },          { "fearUncarried", &Residue::fearUncarried, 0.0f, 3.0f, "strict.flee" },
        { "fearActs", &Residue::fearActs, 0.0f, 1.0f, "strict.flee" },              { "notCarriedShare", &Residue::notCarriedShare, 0.0f, 0.1f, "stand session" },
    };
    static_assert(sizeof(Residue) == std::size(kResidue) * sizeof(float), "every residue has its row");
    // --move-caps <strict,kite,disengage>: what the mover serves on the strict road. The default is strict,kite: H-M3's disengage (and the run of a GoTo) is a later delta.
    struct StrictCaps { bool kite = true, disengage = false; };
    inline StrictCaps g_moveCaps;
    // A FORCED ARM (ModeSet.h): the subject's orders are this rule's instead of the pass's. Per fight, so arms run side by side.
    enum class Force : std::uint8_t { None = 0, Free, Stand, Kite, Disengage };
    enum class StrictPhase : std::uint8_t { None = 0, Pending, Travelling, Standing, Returning, Blocked, Ended };

    // THE FAULT VARIANTS: one mechanism of the pass broken from outside (the pure code has no hook), so a scenario's rule is shown to test it.
    struct MoveFaultRow {
        const char* name; std::uint32_t kinds = 0 /* never offered */; bool terrainBlind = false /* the pass's rays do not see the ground */; bool blind = false /* ... nor the walls */;
        bool reachBlind = false /* who cannot be walked to is not known */; bool noPortals = false /* no place stands in a doorway */; bool noExits = false /* a dead end counts as a way on */;
        bool noRays = false /* no ray is ever answered */; bool stayHolds = false /* "stay" is carried out as a claim on his own place */; bool meleeHands = false /* the pass is told every attack is a blade's */;
        bool noMoveHeal = false /* no heal past its reach is told to the pass */; bool noTrust = false /* no mark is ever kept (S3) */; bool hairTrust = false /* one lived sign distrusts: the 11-17 GATED mistake */; bool noMemory = false /* a blocked leg does not remember its spot */; bool noLight = false /* S4: the light layer and every light fact off */; bool noVeto = false /* nobody is told to want the dark */; bool noDecast = false /* no road ends a light */; bool noNeed = false /* whom and how dark are not read */; bool noWalked = false /* no walk clears a mark (S3 review R6) */; bool fleeWins = false /* strict: the engine's flee beats the order */; bool noReturn = false /* strict: displaced, he is not walked back */; bool kiteRuns = false /* strict: he backs away at his run */; bool disengageFights = false /* strict: a disengage order travels like a goto */; bool dodgeOut = false /* strict: a dodge is not cut at the leash */; bool noLeash = false /* strict: ch.23's leash does not win over the order */; bool gotoRuns = false /* strict: a GoTo has his run without the disengage cap */; bool noOrders = false /* strict: every order is Free */; bool noBlock = false /* strict: a block is never counted, the order stands for ever */; bool anyLeg = false /* any goal-less leg that ends ends the claim (S2 review F6) */;
    };
    inline constexpr MoveFaultRow kMoveFaults[] = {
        { .name = "none" },
        { .name = "no-heal", .kinds = MV::KindBit(MFO::Iaus::SpotKind::HealReach) },
        { .name = "no-move-heal", .noMoveHeal = true },
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
        { .name = "no-light", .noLight = true },
        { .name = "no-veto", .noVeto = true },
        { .name = "no-decast", .noDecast = true },
        { .name = "no-need", .noNeed = true },
        { .name = "no-walked", .noWalked = true },
        { .name = "flee-wins", .fleeWins = true },
        { .name = "no-return", .noReturn = true },
        { .name = "kite-runs", .kiteRuns = true },
        { .name = "disengage-fights", .disengageFights = true },
        { .name = "dodge-out", .dodgeOut = true },
        { .name = "no-leash", .noLeash = true },
        { .name = "goto-runs", .gotoRuns = true },
        { .name = "no-orders", .noOrders = true },
        { .name = "no-block", .noBlock = true },
        { .name = "any-leg", .anyLeg = true },
    };
    inline const MoveFaultRow* g_moveFault = &kMoveFaults[0];
    [[nodiscard]] inline const MoveFaultRow* MoveFaultByName(std::string_view n) { for (const MoveFaultRow& f : kMoveFaults) if (n == f.name) return &f; return nullptr; }

    // What one follower's claim is doing (by Fight::ctl index).
    struct StrictHand {   // the strict order in hand (StrictWorld.h). A new place claim keeps it
        MV::MoveOrder order; std::uint32_t orderGen = 0; StrictPhase phase = StrictPhase::None; float stopAt = 0.0f; double nextPick = 0.0, notBefore = 0.0, carriedAt = 0.0, retryAt = 0.0, dueBlock = -1.0; std::array<double, 3> blocks{}; std::uint8_t nBlocks = 0;
        bool filed = false;                          // an order was filed before: the next one pays refileStall
        bool notCarried = false;                     // Residue::notCarriedShare: this point never is. carriedAt: when the area carries it, the engine's until then
        MV::MoveOrder next; double nextAt = -1.0;    // Residue::bridgeLag: the order that is not live yet
        double fearSeen = 0.0, fearFree = 0.0;       // the fear effect that was counted, and until when the order is not carried under it
        double actSeen = 0.0, stopUntil = 0.0;       // Residue::shotStop
    };
    struct MoveClaim : StrictHand {
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
        std::vector<MV::RayAsk> asks; std::size_t asked = 0; std::uint32_t legs = 0; double meterAt = -1e9; double winStart = 0.0, winEnd = 0.0; bool open = false;
        std::vector<MFO::Iaus::Trust::Line> trustLog; std::vector<EQ::Vec3> lastAt; EQ::Vec3 playerWas; double playerSeen = 0.0;   // S3: every [env-trust] line of the fight, and where each body was a step ago
        Force force = Force::None; int forceVar = 0; std::uint32_t forceGen = 0; MV::MoveOrder forced; long forceUsed = 0; void (*forceRule)(Fight&, MV::MoveOrder&) = nullptr;   // ModeSet.h
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
