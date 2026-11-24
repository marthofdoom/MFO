#pragma once
// iaus/PartyPicture.h -- WHAT THE MOVEMENT PASS DECIDES FROM (S1, Docs/research/2026-11/movement-plan.md, iaus-sim-v4.md 8.7).
//
// One picture for the whole party, frozen once per decision window. PURE: standard headers only, no engine type, no caller in the DLL yet.
// The holder (the simulator's world, the harness, later the worker in G3) fills the INPUT part, calls MovePass::Freeze, and runs the pass.
// The WORK part is the pass's own. A picture lives across windows: the facts, the path cache and each member's commit carry over.
//
// TO ADD OR CHANGE
//   an input about a member / a foe ...... Member / Foe here, its canonical dump line in MoveDump.h
//   a budget ............................. kMoveBudget (one table)
//   an event that frees a commit ......... Mark
#include "iaus/Core.h"
#include "iaus/EnvQuery.h"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace MFO::Iaus::Move {

    using FormID = std::uint32_t;

    // ── spots ──
    // A spot's identity, stable across windows while the mesh's stamp stands: triangle * 4 + which of its points. A member's own place is kStayBit | FormID.
    using SpotKey = std::uint64_t;
    inline constexpr SpotKey kStayBit = std::uint64_t{ 1 } << 63;
    [[nodiscard]] inline constexpr SpotKey KeyOfPoint(std::int32_t a_tri, int a_sub) { return static_cast<SpotKey>(static_cast<std::uint32_t>(a_tri)) * 4u + static_cast<SpotKey>(a_sub); }
    [[nodiscard]] inline constexpr SpotKey KeyOfStay(FormID a_who) { return kStayBit | a_who; }
    [[nodiscard]] inline constexpr std::uint32_t KindBit(SpotKind a_k) { return 1u << static_cast<unsigned>(a_k); }

    enum Need : std::uint32_t { kNeedPin = 1, kNeedFacing = 2, kNeedGait = 4 };   // what a kind needs of the mover
    struct RoadCaps { bool point = true, hold = true, pin = false, facing = false, gait = false; };   // what the mover can do NOW (the ABI today: point and hold)

    struct Spot {
        SpotKey key = 0; Env::Vec3 p; std::int32_t tri = -1;
        std::uint32_t kinds = 0;                       // KindBit
        std::array<std::uint64_t, 2> portals{};        // the doorways a body here stands in. 0 = none
        std::uint8_t exits = 0;                        // routes out (octants) not toward a foe
    };

    // ── facts shared by the party: "that actor sees that spot", keyed (spot, actor) ──
    struct PairFact { Iaus::Tri sees = Iaus::Tri::Unknown; double t = 0.0; std::int32_t actorTri = -1; Env::Vec3 actorAt, spotAt; };
    using PairKey = std::pair<SpotKey, FormID>;
    struct RayAsk    { SpotKey spot = 0; Env::Vec3 p; FormID viewer = 0; bool operator==(const RayAsk& o) const { return spot == o.spot && viewer == o.viewer; } };
    struct RayAnswer { SpotKey spot = 0; FormID viewer = 0; Iaus::Tri sees = Iaus::Tri::Unknown; };

    // ── members and foes ──
    enum Mark : std::uint32_t {   // a REAL event since the last pass: it frees a committed member for the next pass, it never runs one
        kMarkTargetGone = 1, kMarkPathBlocked = 2, kMarkSightLost = 4, kMarkHitHard = 8, kMarkOrder = 16, kMarkCombatEnd = 32, kMarkArrived = 64,
        kMarkActionChanged = 128 };
    struct Commit {
        bool on = false;
        SpotKey key = 0; Env::Vec3 p; std::int32_t tri = -1; std::uint32_t kinds = 0;
        RankedAction act;                              // the action the spot was chosen for
        RankedAction held;                             // the action he held when it was chosen (top[0]): a change of it is an event
        double at = 0.0;
    };
    struct Member {
        FormID id = 0; Env::Vec3 p; std::int32_t tri = -1; float ageSec = 0.0f;
        bool  decides = true;                          // false: a body of the party the pass plans around (the player)
        std::array<RankedAction, 3> top{}; std::uint8_t nTop = 0;   // TopRanked: held action first
        std::uint8_t classId = 0xFF;                   // which Profile (ClassProfiles)
        std::uint32_t kinds = 0xFFFFFFFFu;             // SpotKindsFor(his ClassGate)
        Env::Walker who; float speed = 370.0f;
        Env::Vec3 anchor; float leash = 0.0f;          // 0 = no leash
        bool  wantsDark = false;                       // S4: read only for SpotFacts::light
        Commit commit; std::uint32_t marks = 0;
        // frozen by MovePass::Freeze
        std::array<std::int32_t, 2> threats{ -1, -1 }; // his nearest foes that target him, indexes into PartyPicture::foes
        std::array<std::uint64_t, 2> portals{}; std::uint8_t exits = 0;
        bool  fixed = false;                           // committed and unmarked: a fixed assignment this window
    };
    struct Foe { FormID id = 0; Env::Vec3 p; std::int32_t tri = -1; float ageSec = 0.0f; FormID target = 0; float faceX = 0.0f, faceY = 0.0f; };   // facing: unit, 0 0 = unknown
    struct Area { Env::Vec3 p; float r = 0.0f; };                    // an active hostile area
    struct LightAt { SpotKey key = 0; float level = 0.0f; };         // INPUT HOOK (S4 / G6): a sampled level at a spot. Absent = unknown, never dark

    // ── budgets. THE ONE TABLE ──
    // 32 facts is the RATE of new sight answers a window buys. 24 spots is the POOL whose sight is asked at all. A firing spot needs its target's fact and up to two
    // threats': 32 facts make 10 to 32 fresh spots a window, and facts are reused for as long as both ends stand, so 16 to 24 spots stay answered (the plan's section 2
    // item 4 and the brief's number are the same budget from two sides). The other pooled spots are scored with sight unknown. [probe-ray] replaces factsPerWindow.
    struct MoveBudget {
        int   spots = 64;            // the party's pool
        int   answeredSpots = 24;    // pooled spots whose sight is asked
        int   factsPerWindow = 32;   // (spot, actor) sight questions a window
        int   paths = 80;            // path searches a window, after the cache
        int   triples = 1920;        // (member, action, spot) scored
        float sliceMs = 3.0f;        // worker time a pump tick gives phase 1
        int   gatherTris = 192;      // triangles a flood closes
        int   foeFloods = 8;         // foes whose own walk is flooded (kite)
        int   perGen = 3;            // spots a kind keeps for one member and action
        int   geomPerGen = 64;       // ... of which this many may be tested with geometry rays
        int   pathSamples = 4;       // points of a walk tested for exposure
    };
    inline constexpr MoveBudget kMoveBudget{};
    // What a budget cost this window. The pass counts, the holder logs (once per fight).
    struct MoveCounts { std::uint32_t spotsDropped = 0, pathsSkipped = 0, asksShort = 0, asksCarried = 0, triplesDropped = 0, generated = 0, paths = 0, triples = 0, unfinished = 0; };   // unfinished: Phase3 ran before Phase1 was done

    struct PathFact {
        bool reachable = false; float length = 0.0f, climb = 0.0f, drop = 0.0f;
        Env::Vec3 from; std::array<Env::Vec3, 8> at{}; std::uint8_t nAt = 0;
        std::array<std::uint64_t, 6> portals{}; std::uint8_t nPortals = 0;
    };
    struct PathKey { FormID who = 0; std::int32_t fromTri = -1; SpotKey to = 0; std::int32_t toTri = -1; bool actor = false; auto operator<=>(const PathKey&) const = default; };

    // The pass's working memory for one window. Nothing outside MovePass.cpp / Spots.cpp reads it.
    struct Work {
        enum class Stage : std::uint8_t { Flood, Generate, Pool, Paths, Asks, Done } stage = Stage::Done;
        std::size_t cursor = 0;
        struct Flood { FormID who = 0; std::vector<std::pair<std::int32_t, float>> d; [[nodiscard]] float At(std::int32_t a_tri) const; };   // sorted by triangle. < 0 = not reached
        std::vector<Flood> memberFlood, foeFlood;      // memberFlood by party index (empty for who does not decide), foeFlood by foe index
        struct Hit { Spot s; std::int32_t member = -1; std::int32_t rank = 0; };
        std::vector<Hit> hits;
        struct GenUnit { std::uint32_t member, action, kind; };
        std::vector<GenUnit> gen;
        struct PathUnit { std::uint32_t member; std::int32_t spot; FormID actor; std::int32_t near; };   // spot < 0: the walk to the actor (target reachability)
        std::vector<PathUnit> pathUnits;
        struct Pair { std::uint32_t member; std::int32_t spot; PathFact path; float exposedSec = 0.0f; };
        std::vector<Pair> pairs;                       // reachable (member, spot)
        std::map<std::pair<FormID, FormID>, Iaus::Tri> reach;   // (member, target): can he walk to it
        std::vector<RayAsk> asked;                     // filed this window, not answered yet
    };

    struct PartyPicture {
        // INPUT, filled by the holder before Freeze
        std::uint64_t envStamp = 0;                    // View::stamp the caches below were made on (Freeze keeps it)
        double t = 0.0;
        std::vector<Member> party;                     // Freeze sorts by FormID
        std::vector<Foe>    foes;
        std::vector<Area>   areas;
        std::vector<LightAt> light;
        RoadCaps caps;
        // carried across windows
        std::map<PairKey, PairFact> facts;
        std::map<PathKey, PathFact> paths;
        std::vector<RayAsk> carried;                   // asked, never answered: asked first next window
        std::vector<std::uint8_t> wallVert;            // per vertex: an end of a Wall edge (the doorway test), for envStamp
        std::uint32_t window = 0;
        // this window
        std::vector<Spot> pool;                        // sorted by key
        MoveCounts counts;
        Work work;
        // Everything the pass remembers (ResetAllState's half, and a new cell's).
        void Clear() { *this = PartyPicture{}; }
    };

    struct Assignment {
        FormID who = 0;
        std::int32_t spot = -1;                        // index into PartyPicture::pool, -1 = stay
        SpotKey key = 0; Env::Vec3 p; std::int32_t tri = -1; std::uint32_t kinds = 0;
        Family action = Family::kCount; std::uint32_t form = 0, target = 0;   // kCount: he had no ranked action
        float  score = 0.0f;
        bool   kept = false;                           // a commit that stands: the executor makes no call
        RankedAction act;
    };
}
