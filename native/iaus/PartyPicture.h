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
#include "iaus/EnvTrust.h"
#include "iaus/Light.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
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
    inline constexpr std::int32_t kAnyTri = -2;        // a fact whose viewer's triangle is not known: only how far he moved counts
    // t / viewerAt / viewerTri: the window's freeze the question was filed at.
    struct RayAsk    { SpotKey spot = 0; Env::Vec3 p; FormID viewer = 0; double t = 0.0; Env::Vec3 viewerAt; std::int32_t viewerTri = -1;
                       bool operator==(const RayAsk& o) const { return spot == o.spot && viewer == o.viewer; } };
    // t >= 0: when the ray was cast and where the viewer stood (a road that answers later than its window says so). t < 0: as the question was FIRST filed.
    struct RayAnswer { SpotKey spot = 0; FormID viewer = 0; Iaus::Tri sees = Iaus::Tri::Unknown; double t = -1.0; Env::Vec3 viewerAt{}; };

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
        bool  wantsDark = false;                       // S4 (Light::WantsNow or a sneak class): the light facts are read for him, and for nobody else
        float sneakSkill = -1.0f;                      // S4: his Sneak, perks included (DetectIn::skillT). < 0 = not read: spot_detected does not apply
        bool  invisible = false;
        Commit commit;
        std::uint32_t marks = 0;                       // the holder's INBOX: set at any time, also between Freeze and Adopt. Freeze takes it, Adopt never touches it
        // frozen by MovePass::Freeze
        std::uint32_t seen = 0;                        // the marks this window decides on (the inbox it took + what Freeze found). Adopt clears it
        std::array<std::int32_t, 2> threats{ -1, -1 }; // his nearest foes that target him, indexes into PartyPicture::foes
        std::array<std::uint64_t, 2> portals{}; std::uint8_t exits = 0;
        bool  fixed = false;                           // committed and unmarked: a fixed assignment this window
    };
    struct Foe { FormID id = 0; Env::Vec3 p; std::int32_t tri = -1; float ageSec = 0.0f; FormID target = 0; float faceX = 0.0f, faceY = 0.0f;   // facing: unit, 0 0 = unknown
        float skill = -1.0f; };                        // S4: its perception skill (DetectIn::skillO). < 0 = not read
    struct Area { Env::Vec3 p; float r = 0.0f; };                    // an active hostile area
    struct LightAt { SpotKey key = 0; float level = 0.0f; };         // A POINT SAMPLER's answer at a pooled spot (S4 / G6, probe L1): filled after Phase1, before Phase3. Absent = the field below answers, or nobody (unknown, never dark)

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
        int   gatherTris = 2048;     // triangles a flood closes (a bound on the worst mesh: spotRadius 1500 of 64-unit cells is about 3400). A flood it cuts is counted
        int   lateAsks = 64;         // filed questions of earlier windows an answer may still come for
        int   foeFloods = 8;         // foes whose own walk is flooded (kite)
        int   perGen = 3;            // spots a kind keeps for one member and action
        int   geomPerGen = 64;       // ... of which this many may be tested with geometry rays
        int   pathSamples = 4;       // points of a walk tested for exposure
    };
    inline constexpr MoveBudget kMoveBudget{};
    inline constexpr std::size_t kExitFloods = 256;   // memory only: past it the cache is emptied
    // What a budget cost this window. The pass counts, the holder logs (once per fight).
    struct MoveCounts { std::uint32_t spotsDropped = 0, pathsSkipped = 0, asksShort = 0, asksCarried = 0, triplesDropped = 0, generated = 0, paths = 0, triples = 0,
        unfinished = 0,                                // Phase3 ran before Phase1 was done
        floodsShort = 0,                               // a flood stopped at gatherTris inside spotRadius
        viewChanged = 0,                               // the mesh changed under the window: phase 1 was dropped, everyone uncommitted stays
        lateDropped = 0; };                            // filed questions given up on

    struct PathFact {
        bool reachable = false; float length = 0.0f, climb = 0.0f, drop = 0.0f;
        Env::Vec3 from; std::array<Env::Vec3, 8> at{}; std::uint8_t nAt = 0;
        std::array<std::uint64_t, 6> portals{}; std::uint8_t nPortals = 0;
        std::array<Env::Vec3, 16> via{}; std::uint8_t nVia = 0;   // the walk as points at most 128 apart (farther on a long one), its end the last: what trust reads
    };
    struct PathKey { FormID who = 0; std::int32_t fromTri = -1; SpotKey to = 0; std::int32_t toTri = -1; bool actor = false; auto operator<=>(const PathKey&) const = default; };

    // CACHES OF EXACT INPUTS (S3, the cost of S2). Nothing here changes a result: each value is the one a fresh computation gives, for the same inputs to the last
    // bit, and all of it is dropped when the view's stamp moves. A copied picture (a rollout) shares them; a shared one is never written (Own()).
    struct MeshFacts { std::vector<Env::Vec3> cen; std::vector<std::array<float, 3>> in, out; };   // per triangle: its middle, and per edge the way middle -> edge middle -> the neighbour's middle
    struct ExitMemo { float exitU = -1.0f; std::vector<std::uint16_t> open; };                     // per spot point (KeyOfPoint), for a plain Walker: the open octants (OpenOctants), 0xFFFF = not worked out

    // The pass's working memory for one window. Nothing outside MovePass.cpp / Spots.cpp reads it.
    struct SpotPoint { SpotKey key = 0; Env::Vec3 p; std::int32_t tri = -1; float walk = 0.0f; float lit = 0.0f; };   // lit: what its known light adds to its rank (S4, for who wants the dark)
    struct Work {
        enum class Stage : std::uint8_t { Flood, Generate, Merge, Place, Select, Offer, Queue, Paths, Asks, Done } stage = Stage::Done;
        std::size_t cursor = 0;
        struct Flood { FormID who = 0; std::vector<std::pair<std::int32_t, float>> d; float bound = 0.0f; bool cut = false; [[nodiscard]] float At(std::int32_t a_tri) const; };   // sorted by triangle. < 0 = not reached, so at least `bound` away
        struct Walked { FormID who = 0; std::shared_ptr<const Flood> f; [[nodiscard]] const Flood& Get() const { static const Flood none; return f ? *f : none; } };
        std::vector<Walked> memberFlood, foeFlood;     // memberFlood by party index (empty for who does not decide), foeFlood: the foes whose walk is asked
        // CheckExit's flood of a spot, by (spot, WalkerBits) and for its exact point and reach: the same for every member, action and window of one stamp
        struct ExitFlood { Env::Vec3 p; float radius = -1.0f; std::shared_ptr<const Flood> f; };
        using ExitFloods = std::map<std::pair<SpotKey, std::uint32_t>, ExitFlood>;
        struct Hit { Spot s; std::int32_t rank = 0; };
        std::vector<Hit> hits;                         // what the generators found; after Stage::Merge one per key
        // A spot is SHARED, what it is good for is not: a kind is a claim about ITS member, action, target and threats. Every pooled spot is asked again per
        // (member, action), so a retreat of one member is never another's walk toward his own attacker. Sorted by (spot, member, action).
        struct Offer { std::int32_t spot; std::uint32_t member; std::uint8_t action; std::uint32_t kinds; };
        std::vector<Offer> offers;
        [[nodiscard]] std::uint32_t Offered(std::int32_t a_spot, std::uint32_t a_member, std::uint8_t a_action) const;   // 0 = not his
        std::vector<std::vector<SpotPoint>> points;    // by party index: the candidate points of his gather, once for all his (action, kind) units
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
        Light::Field lightField;                       // S4: what the actors' own levels (and any sampler) told lately. The holder adds between windows, Freeze ages it
        bool exterior = false;                         // S4: for the detection formula
        Light::DetectGmst detect;                      // S4: the list's own numbers
        RoadCaps caps;
        Env::SightTuning sight;                        // the geometry rays' rules (what is worth asking). Shipped = Env::kSightTuning; the sim's sight layers are switches
        // THE TRUST TABLE (S3). The holder writes it whenever it lives something (Trust::Leg, Note, Walked), and takes `log` to print. The pass reads the copy
        // Freeze makes, so nothing that happens between Freeze and Adopt changes a window.
        Trust::Table trust;
        Trust::Table trustNow;                         // frozen
        std::vector<float> avoid;                      // per triangle: what a route pays there (1 = plain). Empty while everything is trusted
        std::uint32_t trustGen = 0;
        // carried across windows
        std::map<PairKey, PairFact> facts;
        std::map<PathKey, PathFact> paths;
        std::vector<RayAsk> carried;                   // wanted, not answered: asked first next window
        std::vector<RayAsk> late;                      // filed in an earlier window, not answered yet, oldest first: an answer is still taken
        std::vector<std::uint8_t> wallVert;            // per vertex: an end of a Wall edge (the doorway test), for envStamp
        std::shared_ptr<const MeshFacts> mesh;         // for envStamp
        std::shared_ptr<ExitMemo> exitMemo;
        mutable std::shared_ptr<Work::ExitFloods> exitFlood;   // bounded (kExitFloods); copied before a write while another picture shares it
        struct Still { FormID who = 0; Env::Vec3 p; std::int32_t tri = -1; std::uint32_t bits = 0; float radius = -1.0f; std::shared_ptr<const Work::Flood> flood; float exitU = -1.0f; std::uint8_t open = 0; };
        std::vector<Still> still;                      // a body that has not moved at all: its walk and its ways out are last window's
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
