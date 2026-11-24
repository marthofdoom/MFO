#pragma once
// iaus/EnvQuery.h -- WHAT THE PURE LAYER MAY ASK ABOUT THE WORLD (sim v4 batch W, Docs/research/2026-11/iaus-sim-v4.md 2.2, 2.4, 8.2).
//
// PURE: standard headers only, no engine type. ONE implementation for the game and the simulator, so the two can never
// fork. NOT WIRED INTO THE DLL YET: nothing in the game calls it. The simulator (tools/iaus_sim/Env.h) builds a View over
// an extracted .mfoenv file. The game will build the same View from the ACTIVE navmesh of the loaded cells (batch N1), on
// the main thread, as an immutable snapshot of plain arrays.
//
// WHAT IS HERE
//   View          plain spans over vertices, triangles, doors, walls, a point-location grid and a stamp.
//   FindPath      route search over triangles (A*), corners by the funnel, drops as one-way breaks, a closed door as a
//                 cost or as a block. The ONE reader of "can he get there and how far is it".    (PathSearch.cpp)
//   WalkLine      a straight walk over the mesh that stops at the first edge it may not cross.
//   CastRay       geometry rays for line of sight. IN THE GAME THE ENGINE'S RAYS ARE THE AUTHORITY (marth 11-23: "I would
//                 lean on the rays"). This ray only chooses which engine rays are worth asking. In the simulator it stands
//                 in for the engine's.
//   BuildGrid / BuildWalls / LabelComponents   the derived tables, built once per snapshot.
//
// TO ADD OR CHANGE
//   a link kind's meaning ........ kLinkKinds below (one table)
//   a path cost .................. PathTuning (one table), read only in PathSearch.cpp
//   a sight rule or threshold .... SightTuning (one table), read only in EnvQuery.cpp
//   a geometric fact ............. a function here, filled in one place
//
// LINK KINDS (tools/envextract README, decoded from the data's direction and the community enum, NOT observed in game):
//   0 two-way walk edge. 1 ledge UP, never walked. 2 one-way DROP down. 3 enable / disable portal, closed by default.
// marth 11-23: a drop is used only by a walker whose abilities say so, and that default is OFF until the field probe has
// seen a follower take one.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace MFO::Iaus::Env {

    struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };

    // What lies across a triangle's edge.
    enum class Edge : std::uint8_t {
        Wall = 0,   // nothing walkable: the mesh ends here
        Walk,       // an ordinary neighbour, both ways
        Drop,       // a one-way drop DOWN to nb (the landing triangle need not touch this edge)
        LedgeUp,    // the reverse of a drop: never an edge for a walker
        Portal,     // enable / disable portal: closed unless View::portalsOpen
        Border,     // the snapshot ends here (a cell that is not loaded, the window's edge). Not a wall for sight
    };
    // The file's LINK.kind -> Edge. The ONE table of what a link kind means.
    inline constexpr Edge kLinkKinds[4] = { Edge::Walk, Edge::LedgeUp, Edge::Drop, Edge::Portal };

    enum TriFlag : std::uint8_t { kTriWater = 1, kTriPreferred = 2, kTriNoLarge = 4 };

    struct Tri {
        std::uint32_t v[3] = { 0, 0, 0 };          // vertex indexes. Edge k runs v[k] -> v[(k + 1) % 3]
        std::int32_t  nb[3] = { -1, -1, -1 };      // the triangle across edge k (Walk, Drop, LedgeUp, Portal), else -1
        Edge          kind[3] = { Edge::Wall, Edge::Wall, Edge::Wall };
        std::uint8_t  flags = 0;                   // TriFlag
        std::int32_t  door = -1;                   // index into View::doors when a door stands on this triangle
    };

    enum class DoorState : std::uint8_t { Open = 0, Closed, Locked };
    // One row per triangle a door stands on. id: the door's own reference, triangles that share it open together. leaf: the builder knows where
    // the door's leaf stands (a to b, floor z0 to top z1), and sight is tested against that instead of against the whole triangle.
    struct Door { std::int32_t tri = -1; DoorState state = DoorState::Closed; std::uint32_t key = 0; std::uint32_t id = 0;
                  bool leaf = false; float ax = 0, ay = 0, bx = 0, by = 0, z0 = 0, z1 = 0; };

    // A sight blocker raised on a mesh edge. The interior of the mesh is on the LEFT of a -> b. At a point of the edge
    // whose height is ez the wall stands from ez + lo to ez + hi.
    //   Solid : a wall. It blocks unless the ray is back on the mesh within SightTuning::gapU (furniture, a pillar).
    //   Face  : a cliff face under a ledge, or over the foot of one. It blocks whatever follows.
    enum class WallKind : std::uint8_t { Solid = 0, Face };
    struct Wall { float ax = 0, ay = 0, az = 0, bx = 0, by = 0, bz = 0, lo = 0, hi = 0; WallKind kind = WallKind::Solid; };

    // Point location and ray walking: a uniform grid over x and y, rows of triangle and wall indexes per cell.
    struct Grid {
        float x0 = 0.0f, y0 = 0.0f, cell = 256.0f; int nx = 0, ny = 0;
        std::vector<std::uint32_t> triStart, triItems, wallStart, wallItems;   // start has nx * ny + 1 entries
        [[nodiscard]] int CellOf(float a_x, float a_y) const;                  // -1 = outside
    };

    struct View {
        std::span<const Vec3> verts;
        std::span<const Tri>  tris;
        std::span<const Door> doors;
        std::span<const Wall> walls;
        std::span<const std::uint32_t> weak;   // per triangle: an id shared by everything any link joins. Empty = not labelled
        const Grid*   grid = nullptr;
        bool          interior = true;
        bool          portalsOpen = false;     // something enabled the enable / disable portals
        std::uint64_t stamp = 0;               // changes when a door or a triangle changes: every cache keys on it
    };

    // Who walks.
    struct Walker {
        float radius = 35.0f;
        bool  opensDoors = true;     // a closed door costs him its open time. false = it blocks him
        bool  unlocks = false;       // he has the key or a pick road
        bool  large = false;         // closed out of NoLargeCreatures triangles
        bool  drops = false;         // may take a Drop edge (iaus/Traversal.h will fill this, batch M2)
        float maxDrop = 0.0f;        // the deepest drop he takes, units
    };

    // THE PATH COSTS. One table. Each value is a MODEL until the field leg log (travelled length against FindPath's) fits it.
    struct PathTuning {
        float preferredMult = 0.85f;   // length x this on a preferred-path triangle (MODEL: the engine prefers them)
        float waterMult     = 1.8f;    // followers wade (README "Water"): a cost, not a wall
        float doorOpenU     = 370.0f;  // a closed door he can open: about a second at the run speed
        float unlockU       = 1500.0f; // a locked door he can unlock
        float dropBaseU     = 150.0f;  // a drop: the landing, plus dropPerU per unit of height
        float dropPerU      = 0.5f;
        float blockPenalty  = 1.0e6f;  // an edge he cannot open. A path that needs one is BLOCKED there
        float cornerInset   = 0.6f;    // corners stand this share of his radius inside the portal's end
        int   maxExpand     = 0;       // triangles closed before the search gives up (0 = no cap)
    };
    inline constexpr PathTuning kPathTuning{};

    // THE SIGHT RULES. One table. The geometry layer reads windows, railings and low walls as solid and misses thin
    // statics on walkable floor (iaus-sim-v4.md 8.5). Its error against the engine's rays is not measured yet.
    struct SightTuning {
        float gapU         = 80.0f;    // interior: off-mesh space narrower than this is furniture, not a wall
        float gapExteriorU = 256.0f;   // exterior: a rock or a tree's hole. ASSUMPTION
        float wallH        = 280.0f;   // a wall stands this high over its floor edge
        float wallBelow    = 24.0f;    // and this far under it
        float stepH        = 64.0f;    // two floors closer in height than this are one level
        float probeU       = 160.0f;   // how far past an edge the builder looks for the next floor
        float cliffMaxH    = 4000.0f;  // a higher floor farther up than this is not this edge's cliff
        bool  terrain      = true;     // rays are also tested against the mesh's own triangles (a ridge, a floor above)
        bool  walls        = true;
    };
    inline constexpr SightTuning kSightTuning{};

    enum class Corner : std::uint8_t { Walk = 0, DropStart, DropLand, Goal };
    struct PathPoint { Vec3 p; std::int32_t tri = -1; Corner kind = Corner::Walk; };

    struct Path {
        std::vector<PathPoint> pts;        // the corners after the start, the last one is the goal (or where the path ends)
        float length = 0.0f;               // walked length of pts from the start, drops included as their straight fall
        bool  reachable = false;           // pts ends AT the goal
        std::int32_t blockDoor = -1;       // the door that ended it (reachable false, the path stops in front of it)
        bool  blocked = false;             // ended at something he cannot open
        std::uint16_t drops = 0;
        float climb = 0.0f, fall = 0.0f;   // summed height gained and lost on walk edges
        std::uint32_t expanded = 0;        // triangles the search closed (the cost read)
        void Clear() { pts.clear(); length = 0.0f; reachable = false; blockDoor = -1; blocked = false; drops = 0; climb = fall = 0.0f; expanded = 0; }
    };

    struct PathQuery { Vec3 from, to; std::int32_t fromTri = -1, toTri = -1; Walker who; };

    // Reused search memory. One per thread. Nothing in it survives a call as a result.
    struct Scratch {
        std::vector<float> g; std::vector<std::int32_t> parent; std::vector<std::uint8_t> state; std::vector<std::uint32_t> mark; std::vector<Vec3> entry;
        std::uint32_t gen = 0;
        struct Node { float f; std::int32_t tri; };
        std::vector<Node> heap;
        struct Step { std::int32_t tri; std::int8_t edge; };   // edge = the edge of the PREVIOUS triangle he came through
        std::vector<Step> corridor;
        struct Portal { Vec3 l, r; std::int32_t tri; };
        std::vector<Portal> portals;
        std::vector<std::uint32_t> seen;                       // CastRay: walls already tested
        struct Cross { float t; bool leaving; };
        std::vector<Cross> cross;
    };

    // ── point location ──
    [[nodiscard]] float HeightAt(const View& a_v, std::int32_t a_tri, float a_x, float a_y);
    // The triangle whose x, y holds the point, nearest in height to a_p.z. -1 = none.
    [[nodiscard]] std::int32_t TriAt(const View& a_v, Vec3 a_p);
    // The nearest point of the mesh within a_maxDist (height counts double). Returns the triangle, -1 = none.
    [[nodiscard]] std::int32_t NearestTri(const View& a_v, Vec3 a_p, float a_maxDist, Vec3& a_on);
    [[nodiscard]] Vec3 Centroid(const View& a_v, std::int32_t a_tri);
    // How far (x, y) lies outside the triangle, in x, y. 0 = inside.
    [[nodiscard]] float OutsideBy(const View& a_v, std::int32_t a_tri, float a_x, float a_y);

    // ── a straight walk ──
    struct Walk { Vec3 end; std::int32_t tri = -1; bool reached = false; Edge hit = Edge::Wall; std::int32_t hitTri = -1; std::int8_t hitEdge = -1; std::int32_t door = -1; };
    // From a_from (inside a_tri) toward a_to in x, y. Crosses Walk edges only. A closed door he would step onto stops it
    // (door = its index), and so does a triangle his body class is closed out of.
    [[nodiscard]] Walk WalkLine(const View& a_v, std::int32_t a_tri, Vec3 a_from, Vec3 a_to, const Walker& a_who);

    // ── route search (PathSearch.cpp) ──
    // false = no usable path at all (a_out.pts empty). true with reachable false = a partial path: it ends at the block, or
    // at the nearest point the search reached.
    bool FindPath(const View& a_v, const PathQuery& a_q, Scratch& a_s, Path& a_out, const PathTuning& a_t = kPathTuning);

    // ── sight ──
    enum class Blocker : std::uint8_t { None = 0, Wall, Face, Terrain, Door };
    struct Sight { bool clear = true; Blocker by = Blocker::None; float t = 1.0f; };
    [[nodiscard]] Sight CastRay(const View& a_v, Vec3 a_from, Vec3 a_to, Scratch& a_s, const SightTuning& a_t = kSightTuning);

    // ── derived tables, built once per snapshot ──
    void BuildGrid(Grid& a_g, std::span<const Vec3> a_verts, std::span<const Tri> a_tris);
    void BuildWalls(std::vector<Wall>& a_out, const View& a_v, const SightTuning& a_t = kSightTuning);   // needs a_v.grid with its triangles
    void IndexWalls(Grid& a_g, std::span<const Wall> a_walls);
    // walk: joined by Walk edges only (doors count as open). weak: joined by any link. Id 0 is the largest by triangle count.
    void LabelComponents(std::span<const Tri> a_tris, std::vector<std::uint32_t>& a_walk, std::vector<std::uint32_t>& a_weak);

}
