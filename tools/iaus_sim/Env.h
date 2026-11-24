#pragma once
// iaus_sim/Env.h -- SIM V4: a real environment under a fight (Docs/research/2026-11/iaus-sim-v4.md 1, 2). HOST-ONLY.
//
// One immutable SimEnv per loaded MFOENV v2 file (tools/envextract/README.md is the contract, envfile.h the reader): an
// extracted place from ~/.cache/mfo-env, or one of the hand-made fixtures of tools/iaus_sim/envs/. A Fight holds a POINTER
// to it and its own door states, so a Fight stays a plain copyable value (the oracle's rollouts copy a pointer).
//
// The geometry questions (route, straight walk, ray) are the game's own pure files, native/iaus/EnvQuery.h and
// PathSearch.cpp, compiled in as iaus/Generate.cpp is. Nothing here answers one itself.
//
// WHAT THE LOADER DOES WITH THE FILE
//   meshes -> one triangle array with global indexes. Internal neighbours are Walk edges. An external edge takes its LINK
//   row's kind through MFO::Iaus::Env::kLinkKinds (0 walk, 1 ledge up, 2 drop, 3 portal). A link whose target mesh is not in
//   the file is a Border (the window ends). A stale link is a wall.
//   DOOR rows -> a gating door when its ref is a plain door that does not start disabled (closed, or locked). A teleport
//   door is an ENTRY, not a gate. REFS kind 5 that do not start disabled -> actor spawn points.
//   Extra sections are tolerated (the reader skips unknown tags). The sim-only DFCT section of a fixture (envs/make_fixtures.py)
//   marks edges the TRUTH does not have while the planner's mesh still does (iaus-sim-v4.md 8.9).
//   Components are labelled here (LabelComponents), over the resolved global links, and the file's TCMP is not read.
//
// ANOMALOUS PLACES ARE SKIPPED, with a count (marth 11-23): a file that fails the reader, has no triangle, or whose largest
// walk component is too small to hold a fight. The sim does not study broken places.
#include "iaus/EnvQuery.h"
#include "envextract/envfile.h"
#include "iaus_sim/Json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Sim {

    namespace EQ = MFO::Iaus::Env;

    // THE BODY AND FOLLOWING MODEL'S NUMBERS (iaus-sim-v4.md 2.3). One table. Every value is a MODEL or an ASSUMPTION until the
    // field leg log calibrates it (calib_path.py prints the field's rates beside the lab's).
    struct EnvTuning {
        float radius = 30.0f, radiusLarge = 60.0f;   // body circles. MODEL: a humanoid capsule is about 30 units
        float largeMass = 2.0f;                      // a body heavier than this is "large" (NoLargeCreatures triangles are closed to it)
        float repathSec = 0.25f;                     // a mover asks for a new route at most this often while he has one
        float retrySec = 1.0f;                       // after a BLOCKED / unreachable end he asks again this much later
        float doorOpenSec = 1.0f;                    // a closed door he can open (MODEL: the engine's door animation)
        float yieldSec = 0.6f;                       // ASSUMPTION: a friend in his way lets him through after this long
        float passSec = 1.0f;                        // and he may overlap that friend for this long
        float blockSec = 2.0f;                       // no way round a body or a door for this long = the leg ends BLOCKED (Movement Blocked)
        float stuckSec = 2.0f, stuckU = 30.0f;       // moved under stuckU in stuckSec OF WANTING TO MOVE = stuck (iaus-sim-v4.md 2.5). The window counts wanted seconds: a swing or a stagger between two steps neither feeds it nor clears it
        float wantGapSec = 3.0f;                     // ASSUMPTION: not asked to move for longer than this = he stood by choice, and the stuck window begins again (review F8: any gap of three steps used to clear it)
        float legGoalU = 200.0f;                     // a goal that moved farther than this starts a new leg
        float oscSec = 3.0f;                         // a new leg heading back within this of the last leg's start is a reversal
        float legMinU = 200.0f;                      // ... when both legs go farther than this (footwork around a foe in reach is not a leg). Above the 160 unit side step of a held caster (World.h), which is footwork: at 150 it was a leg (re-check R1)
        float sightDwellSec = 1.5f;                  // native apmf/OffenseApproach.cpp kSightDwellMs 1500: a walk-in for sight begins / ends only after the verdict has held this long (Fight::SightWalk)
        float backOutU = 100.0f;                     // ASSUMPTION: a back-off that began inside its distance goes on until he is this far past it (Fight::BackOff)
        float awayCommitSec = 2.0f;                  // a back-off that keeps a foe in sight COMMITS to its pathed point for at most this long (a flight re-picks every awayPickSec)
        float blockLapseU = 300.0f;                  // native apmf/OffenseApproach.cpp kBlockMoveUnits: an approach block lapses when he stands this far from where it was made
        float reblockU = 300.0f;                     // LootTuning::gateRepeatU: a block this near his previous block is a re-block. The same radius makes an UNREACHABLE or a STUCK end a repeat of his previous end of that kind (review F8)
        float dropSec = 0.6f;                        // ASSUMPTION: the time a drop takes
        float maxDrop = 600.0f;                      // the deepest drop a walker with the ability takes (the README's p95)
        float eyeFrac = 0.90f, torsoFrac = 0.55f, feetU = 16.0f, bodyH = 120.0f, targetMargin = 48.0f;   // native Sightline.cpp: eye -> feet / torso / head, each ray ends 48 short
        float spawnGroupU = 1500.0f;                 // MODEL: spawn points this near each other are one encounter
        float snapU = 400.0f;                        // a goal off the mesh is taken to the nearest mesh point within this
        float levelU = 24.0f;                        // he is AT a corner or a goal only within this of its height
        float storeyU = 64.0f;                       // a point of "his own storey" lies within this of his height (EQ::SightTuning::stepH)
        float awayU = 320.0f, awayGainU = 40.0f, awayPickSec = 0.5f;   // the fallback point of a backpedal or a flight: how far it looks, the least it must gain, how long a pick stands
    };
    inline EnvTuning g_envT;

    // SIM V4 switches (iaus-sim-v4.md 1.4): each mechanism is ONE switch. `--world v4` turns the defaults on, `--world v3` (and v2)
    // leaves every one off and the lab is v3 draw for draw. drops and traversal stay OFF until the field probe has numbers (marth 11-23).
    struct V4 {
        bool env = false;        // fights are placed in an environment and bodies stand on its mesh
        bool path = false;       // movement follows routes over the mesh (FindPath), doors gate, legs end BLOCKED / STUCK
        bool bodies = false;     // bodies are circles: a doorway holds one, a narrow corridor nobody passes
        bool los = false;        // sight and the line of fire by geometry (walls, terrain, closed doors)
        bool caststart = false;  // rows Harbinger does not seat never start, the never-started release, the hold-off (section 5)
        bool interrupt = false;  // a stagger or a control effect on a charging caster ends the cast
        bool drops = false;      // followers take the navmesh's drop links (NOT observed in game: off)
        bool foedrops = false;   // foes take them
        void AllOff() { *this = V4{}; }
        void Defaults() { env = path = bodies = los = caststart = interrupt = true; drops = foedrops = false; }
        [[nodiscard]] bool Any() const { return env || path || bodies || los || caststart || interrupt || drops || foedrops; }
    };
    inline V4 g_v4;
    // --seat-immunity seen|unseen: does the game's will-work SEE that a foe is immune to paralysis? native cast/WillWork.cpp (c): "Stagger / Paralyze
    // IMMUNITY is not exposed by the engine as a readable bit ... it is only what the MGEF conditions say; the zero-effect hold catches the rest".
    // The lab's foes carry the ImmuneParalysis keyword (immunePar) but its spells carry no conditions, so which it is on a given list is NOT KNOWN here.
    // Default UNSEEN (the game as native describes it: he casts, it lands nothing, twice, and the pair is held). ASSUMPTION, switchable.
    inline bool g_seatImmunitySeen = false;
    inline bool g_seatsOld = false;   // --seats old: the game before Harbinger ABI 46 (Stagger, Disarm, TargetEffect, Paralyze, Reanimate never start). Default: every row seated

    inline EQ::SightTuning g_sightT;   // --sight walls=off|terrain=off|gap=<u>: one layer at a time, so each layer's error can be read alone (2.4)

    // A body's place on the mesh and the route he is on (World.h Body::nav). Plain values: a Fight copy copies it.
    struct Nav {
        std::int32_t tri = -1;                                   // the triangle he stands on
        std::int32_t goalTri = -1; EQ::Vec3 goal; std::uint32_t goalBody = 0;   // what his corners lead to, and who stands there
        std::array<EQ::PathPoint, 6> pts; std::uint8_t n = 0, i = 0; bool reach = false, blocked = false, cut = false;
        double pathAt = -1e9, holdUntil = 0; std::uint64_t stamp = ~0ull;
        double wantAt = -1e9, progAt = 0; float px = 0, py = 0; bool stuckNoted = false; double stuckSince = -1; float stuckWant = 0.0f /* stuck seconds since his last STUCK end */;   // the stuck window
        double blockSince = -1, passUntil = 0; std::uint32_t blocker = 0;                                          // a body in his way
        std::int32_t door = -1; double doorAt = 0;                                                               // the door he is opening
        double dropUntil = 0; EQ::Vec3 dropTo; std::int32_t dropTri = -1;                                          // a drop in flight
        double legAt = -1e9; float legDx = 0, legDy = 0, legSx = 0, legSy = 0, legGx = 0, legGy = 0, legG0x = 0, legG0y = 0, walked = 0; bool legOpen = false, legReach = false, legLong = false, away = false, exact = false /* his last leg ended STUCK: the next route is searched from where he stands */; float legLen = 0; double awayUntil = 0, exactUntil = 0;
        float endX[3] = {}, endY[3] = {}; bool hadEnd[3] = {};   // where his last leg of each kind ended (LegEnd: BLOCKED, UNREACHABLE, STUCK): an end beside it is a repeat
    };

    struct EnvPoint { EQ::Vec3 p; std::int32_t tri = -1; std::uint32_t base = 0; std::uint8_t kind = 0; };

    struct SimEnv {
        std::string name, cls, set, file; bool interior = true, pool = true; std::uint64_t hash = 0;   // hash: of the file's bytes (the library hash of a run is built from these)
        std::vector<EQ::Vec3> verts;
        std::vector<EQ::Tri>  tris;        // the TRUTH: what a body can walk
        std::vector<EQ::Tri>  planTris;    // what a planner reads, when the file carries defects (else empty = the truth)
        std::vector<EQ::Door> doors0;      // the doors as the place starts
        std::vector<EQ::Wall> walls; EQ::Grid grid;
        std::vector<std::uint32_t> walk, weak;
        std::vector<EnvPoint> entries, spawns, refs;   // load doors on the mesh / actor spawn points / containers and loose items (batch L)
        std::vector<float> triArea; float area0 = 0.0f; std::uint32_t tris0 = 0;   // walk component 0
        int defects = 0, plainDoors = 0;   // plainDoors: door triangles joined from REFS by geometry (no DOOR row)

        [[nodiscard]] EQ::View View(const std::vector<EQ::Door>& doors, std::uint64_t stamp, bool plan = false) const {
            EQ::View v; v.verts = verts; v.tris = plan && !planTris.empty() ? std::span<const EQ::Tri>(planTris) : std::span<const EQ::Tri>(tris); v.doors = doors; v.walls = walls;
            v.weak = weak; v.walk = walk; v.grid = &grid; v.interior = interior; v.stamp = stamp; return v;
        }

        // ── the route cache (iaus-sim-v4.md 2.7). A route is asked CENTROID TO CENTROID, so its value depends on the key alone: a miss on
        // any thread computes the same corners, and a run is the same at 1 thread and at 16. The mover swaps the last corner for his own goal. ──
        struct Route { std::array<EQ::PathPoint, 6> pts; std::uint8_t n = 0; bool reach = false, blocked = false, any = false, cut = false; std::int32_t blockDoor = -1; float length = 0.0f; std::uint16_t drops = 0; std::int32_t from = -1, to = -1; std::uint64_t key2 = 0; };
        struct Shard { std::mutex m; std::unordered_map<std::uint64_t, Route> map; };
        static constexpr std::size_t kShardCap = 4096; static constexpr long kRouteCap = 1'500'000;   // about 150 bytes a route: 40 MB a place at most, 220 MB in all
        static inline std::atomic<long> g_routeTotal{ 0 };
        mutable std::array<Shard, 64> cache;
        mutable std::atomic<std::uint64_t> hits{ 0 }, misses{ 0 };
        static std::uint64_t Mix(std::uint64_t z) { z += 0x9E3779B97F4A7C15ULL; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
        static std::uint8_t WalkerBits(const EQ::Walker& w) { return static_cast<std::uint8_t>((w.opensDoors ? 1 : 0) | (w.unlocks ? 2 : 0) | (w.large ? 4 : 0) | (w.drops ? 8 : 0)); }
        [[nodiscard]] Route FindRoute(const std::vector<EQ::Door>& doors, std::uint64_t stamp, std::int32_t from, std::int32_t to, const EQ::Walker& who, bool plan) const {
            const std::uint64_t key2 = stamp * 31u + WalkerBits(who) + (plan ? 64u : 0u);
            const std::uint64_t key = Mix((static_cast<std::uint64_t>(static_cast<std::uint32_t>(from)) << 32) | static_cast<std::uint32_t>(to)) ^ Mix(key2);
            Shard& sh = cache[key & 63];
            { std::lock_guard<std::mutex> lk(sh.m); const auto it = sh.map.find(key); if (it != sh.map.end() && it->second.from == from && it->second.to == to && it->second.key2 == key2) { hits.fetch_add(1, std::memory_order_relaxed); return it->second; } }
            misses.fetch_add(1, std::memory_order_relaxed);
            static thread_local EQ::Scratch scratch; static thread_local EQ::Path path;
            const EQ::View v = View(doors, stamp, plan);
            EQ::PathQuery q; q.fromTri = from; q.toTri = to; q.from = EQ::Centroid(v, from); q.to = EQ::Centroid(v, to); q.who = who;
            Route r; r.from = from; r.to = to; r.key2 = key2;
            if (EQ::FindPath(v, q, scratch, path)) { r.any = true; r.reach = path.reachable; r.blocked = path.blocked; r.blockDoor = path.blockDoor; r.length = path.length; r.drops = path.drops;
                r.n = static_cast<std::uint8_t>(std::min<std::size_t>(path.pts.size(), r.pts.size())); for (std::uint8_t i = 0; i < r.n; ++i) r.pts[i] = path.pts[i];
                r.cut = path.pts.size() > r.pts.size(); }   // a cut route: he asks again at its last kept corner
            // THE CAP (review F12): a shard holds at most kShardCap routes, and all places together at most kRouteCap. Past either the shard is emptied.
            // A route is a pure value of its key, so dropping one changes nothing but time.
            { std::lock_guard<std::mutex> lk(sh.m); if (sh.map.size() >= kShardCap || g_routeTotal.load(std::memory_order_relaxed) >= kRouteCap) { g_routeTotal.fetch_sub(static_cast<long>(sh.map.size()), std::memory_order_relaxed); sh.map.clear(); }
              if (sh.map.emplace(key, r).second) g_routeTotal.fetch_add(1, std::memory_order_relaxed); }
            return r;
        }

        // ── path distance from one triangle to every other, over Walk edges (doors count as open). Placement only. ──
        mutable std::mutex fieldM; mutable std::unordered_map<std::int32_t, std::shared_ptr<const std::vector<float>>> fields;
        [[nodiscard]] std::shared_ptr<const std::vector<float>> Field(std::int32_t from) const {
            { std::lock_guard<std::mutex> lk(fieldM); const auto it = fields.find(from); if (it != fields.end()) return it->second; }
            auto d = std::make_shared<std::vector<float>>(tris.size(), 1e30f); std::vector<EQ::Vec3> at(tris.size());
            using N = std::pair<float, std::int32_t>; std::priority_queue<N, std::vector<N>, std::greater<N>> pq;
            const EQ::View v = View(doors0, 0); (*d)[static_cast<std::size_t>(from)] = 0.0f; at[static_cast<std::size_t>(from)] = EQ::Centroid(v, from); pq.push({ 0.0f, from });
            while (!pq.empty()) { const N n = pq.top(); pq.pop(); const std::size_t ti = static_cast<std::size_t>(n.second); if (n.first > (*d)[ti]) continue;
                const EQ::Tri& T = tris[ti];
                for (int k = 0; k < 3; ++k) { if (T.kind[k] != EQ::Edge::Walk || T.nb[k] < 0) continue; { const std::int32_t dn = tris[static_cast<std::size_t>(T.nb[k])].door; float lt;   // a locked door is a wall for placement: its triangle when it has no leaf, else the walk across its leaf
                        if (dn >= 0 && !doors0[static_cast<std::size_t>(dn)].leaf && doors0[static_cast<std::size_t>(dn)].state == EQ::DoorState::Locked) continue;
                        if (T.door >= 0 && doors0[static_cast<std::size_t>(T.door)].state == EQ::DoorState::Locked && EQ::LeafCrossed(doors0[static_cast<std::size_t>(T.door)], at[ti], { 0.5f * (verts[T.v[k]].x + verts[T.v[(k + 1) % 3]].x), 0.5f * (verts[T.v[k]].y + verts[T.v[(k + 1) % 3]].y), 0.0f }, lt)) continue; }
                    const EQ::Vec3& a = verts[T.v[k]]; const EQ::Vec3& b = verts[T.v[(k + 1) % 3]];
                    const EQ::Vec3 mid{ 0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (a.z + b.z) }; const float g = n.first + std::hypot(mid.x - at[ti].x, mid.y - at[ti].y, mid.z - at[ti].z);
                    const std::size_t ni = static_cast<std::size_t>(T.nb[k]); if (g < (*d)[ni]) { (*d)[ni] = g; at[ni] = mid; pq.push({ g, T.nb[k] }); } } }
            std::lock_guard<std::mutex> lk(fieldM); if (fields.size() > 64) fields.clear(); return fields[from] = d;
        }
    };

    // Why a place was not taken into the pool.
    struct EnvSkip { std::string file, why; };

    inline std::shared_ptr<SimEnv> LoadEnv(const std::string& path, std::string* err = nullptr) {
        mfoenv::Env f; std::string e;
        if (!f.load(path.c_str(), &e)) { if (err) *err = e; return nullptr; }
        auto E = std::make_shared<SimEnv>(); E->file = path;
        { std::ifstream hin(path, std::ios::binary); std::uint64_t h = 1469598103934665603ULL; char buf[65536]; while (hin) { hin.read(buf, sizeof buf); for (std::streamsize i = 0; i < hin.gcount(); ++i) { h ^= static_cast<unsigned char>(buf[i]); h *= 1099511628211ULL; } } E->hash = h; }
        { const std::string& m = f.metaJson(); JP jp{ m }; J j; if (!m.empty() && m[0] == '{') j = jp.val();
          E->name = j["name"].s; E->cls = j["class"].s; E->set = j["set"].s; E->interior = j["interior"].has() ? j["interior"].yes() : true; E->pool = j["pool"].has() ? j["pool"].yes() : true;
          if (E->name.empty()) { const std::size_t s = path.find_last_of('/'); E->name = path.substr(s == std::string::npos ? 0 : s + 1); } }
        std::size_t nv = 0, nt = 0; for (std::uint32_t mi = 0; mi < f.meshCount(); ++mi) { nv += f.mesh(mi).vertCount; nt += f.mesh(mi).triCount; }
        if (nt == 0) { if (err) *err = "no triangle"; return nullptr; }
        E->verts.reserve(nv); E->tris.resize(nt);
        for (std::uint32_t mi = 0; mi < f.meshCount(); ++mi) { const mfoenv::Mesh& M = f.mesh(mi); const mfoenv::Vert* v = f.verts(M); for (std::uint32_t i = 0; i < M.vertCount; ++i) E->verts.push_back({ v[i].x, v[i].y, v[i].z }); }
        for (std::uint32_t mi = 0; mi < f.meshCount(); ++mi) { const mfoenv::Mesh& M = f.mesh(mi); const mfoenv::Tri* t = f.tris(M);
            for (std::uint32_t i = 0; i < M.triCount; ++i) { EQ::Tri& T = E->tris[M.triBase + i];
                for (int k = 0; k < 3; ++k) { T.v[k] = M.vertBase + t[i].v[k]; if (t[i].nb[k] >= 0) { T.nb[k] = static_cast<std::int32_t>(M.triBase) + t[i].nb[k]; T.kind[k] = EQ::Edge::Walk; } }
                T.flags = static_cast<std::uint8_t>((t[i].flags & mfoenv::kWater ? EQ::kTriWater : 0) | (t[i].flags & mfoenv::kPreferred ? EQ::kTriPreferred : 0) | (t[i].flags & mfoenv::kNoLarge ? EQ::kTriNoLarge : 0)); }
            const mfoenv::Link* L = f.links(M);
            for (std::uint32_t i = 0; i < M.linkCount; ++i) { const mfoenv::Link& l = L[i]; if ((l.flags & mfoenv::kNoOwnerEdge) || l.edge > 2) continue;
                EQ::Tri& T = E->tris[M.triBase + l.tri]; if (T.kind[l.edge] == EQ::Edge::Walk) continue;   // an internal neighbour stands: the file says an external edge has nb -1
                if (l.targetMeshIdx < 0) { T.kind[l.edge] = EQ::Edge::Border; continue; }
                if ((l.flags & mfoenv::kStaleTarget) || l.kind > 3) continue;
                T.kind[l.edge] = EQ::kLinkKinds[l.kind]; T.nb[l.edge] = static_cast<std::int32_t>(f.mesh(static_cast<std::uint32_t>(l.targetMeshIdx)).triBase + l.targetTri); } }
        for (std::uint32_t i = 0; i < f.doorCount(); ++i) { const mfoenv::Door& d = f.door(i); if (d.refIdx < 0) continue; const mfoenv::Ref& r = f.ref(static_cast<std::uint32_t>(d.refIdx));
            if ((r.flags & mfoenv::kTeleport) || (r.flags & mfoenv::kStartsDisabled) || r.kind != mfoenv::kDoorRef) continue;
            EQ::Tri& T = E->tris[f.mesh(d.meshIdx).triBase + d.tri]; if (T.door >= 0) continue;
            EQ::Door nd; nd.tri = static_cast<std::int32_t>(f.mesh(d.meshIdx).triBase + d.tri); nd.state = (r.flags & mfoenv::kLocked) ? EQ::DoorState::Locked : EQ::DoorState::Closed; nd.key = r.key; nd.id = r.id;
            T.door = static_cast<std::int32_t>(E->doors0.size()); E->doors0.push_back(nd); }   // one door on several triangles = several rows with one id: they open together (Fight::OpenDoor)
        // sim-only DFCT section: u32 meshIdx; u16 tri; u8 edge; u8 truthKind. The truth loses the edge, the planner's copy keeps it.
        { std::ifstream in(path, std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
          if (b.size() >= 16) { std::uint32_t ns; std::memcpy(&ns, &b[12], 4);
              for (std::uint32_t i = 0; i < ns && 16 + 16 * static_cast<std::size_t>(i + 1) <= b.size(); ++i) { const char* p = &b[16 + 16 * static_cast<std::size_t>(i)]; if (std::memcmp(p, "DFCT", 4) != 0) continue;
                  std::uint32_t off, cnt, rec; std::memcpy(&off, p + 4, 4); std::memcpy(&cnt, p + 8, 4); std::memcpy(&rec, p + 12, 4); if (rec != 8 || static_cast<std::uint64_t>(off) + 8ull * cnt > b.size()) break;
                  E->planTris = E->tris;
                  for (std::uint32_t k = 0; k < cnt; ++k) { std::uint32_t mi; std::uint16_t tri; std::uint8_t edge; std::memcpy(&mi, &b[off + 8 * k], 4); std::memcpy(&tri, &b[off + 8 * k + 4], 2); edge = static_cast<std::uint8_t>(b[off + 8 * k + 6]);
                      if (mi >= f.meshCount() || tri >= f.mesh(mi).triCount || edge > 2) continue; EQ::Tri& T = E->tris[f.mesh(mi).triBase + tri]; T.kind[edge] = EQ::Edge::Wall; T.nb[edge] = -1; ++E->defects; } } } }
        EQ::BuildGrid(E->grid, E->verts, E->tris);
        // IN-CELL DOORS. The file's DOOR rows join only load doors to triangles (measured on the 11-23 library: 165 rows, all teleport). A plain door is
        // a REFS row with no triangle, so it is joined here by geometry: the leaf is the long side of its box through the box's middle, turned by
        // the ref's z rotation, and every floor triangle that leaf crosses is a door triangle. MODEL: a plain door starts CLOSED (the plugin does not
        // say otherwise in what the extractor exports), locked when its ref says so.
        { std::vector<std::uint32_t> have; for (const EQ::Door& d : E->doors0) have.push_back(d.id);
          for (std::uint32_t i = 0; i < f.refCount(); ++i) { const mfoenv::Ref& r = f.ref(i);
            if (r.kind != mfoenv::kDoorRef || (r.flags & (mfoenv::kTeleport | mfoenv::kStartsDisabled)) || std::find(have.begin(), have.end(), r.id) != have.end()) continue;
            const float bx = r.boundsMax[0] - r.boundsMin[0], by = r.boundsMax[1] - r.boundsMin[1]; if (!(bx > 1.0f) || !(by > 1.0f)) continue;
            const float cxm = 0.5f * (r.boundsMin[0] + r.boundsMax[0]), cym = 0.5f * (r.boundsMin[1] + r.boundsMax[1]);
            float ux = std::cos(r.rot[2]), uy = -std::sin(r.rot[2]); if ((bx >= by) != (std::fabs(ux) >= std::fabs(uy))) { const float t = ux; ux = -uy; uy = t; }   // the leaf lies along the box's long side
            const float h = std::min(std::fabs(ux) > 1e-3f ? 0.5f * bx / std::fabs(ux) : 1e9f, std::fabs(uy) > 1e-3f ? 0.5f * by / std::fabs(uy) : 1e9f);
            EQ::Door nd; nd.state = (r.flags & mfoenv::kLocked) ? EQ::DoorState::Locked : EQ::DoorState::Closed; nd.key = r.key; nd.id = r.id; nd.leaf = true;
            nd.ax = cxm - ux * h; nd.ay = cym - uy * h; nd.bx = cxm + ux * h; nd.by = cym + uy * h; nd.z0 = r.boundsMin[2]; nd.z1 = std::max(r.boundsMax[2], r.boundsMin[2] + 120.0f);
            const EQ::Grid& g = E->grid; const int x0 = std::clamp(static_cast<int>((std::min(nd.ax, nd.bx) - g.x0) / g.cell), 0, g.nx - 1), x1 = std::clamp(static_cast<int>((std::max(nd.ax, nd.bx) - g.x0) / g.cell), 0, g.nx - 1);
            const int y0 = std::clamp(static_cast<int>((std::min(nd.ay, nd.by) - g.y0) / g.cell), 0, g.ny - 1), y1 = std::clamp(static_cast<int>((std::max(nd.ay, nd.by) - g.y0) / g.cell), 0, g.ny - 1);
            auto cr = [](float ax, float ay, float bx2, float by2) { return ax * by2 - ay * bx2; };
            for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) { const std::size_t c = static_cast<std::size_t>(y) * g.nx + x;
                for (std::uint32_t k = g.triStart[c]; k < g.triStart[c + 1]; ++k) { EQ::Tri& T = E->tris[g.triItems[k]]; if (T.door >= 0) continue;
                    const EQ::Vec3* p[4] = { &E->verts[T.v[0]], &E->verts[T.v[1]], &E->verts[T.v[2]], &E->verts[T.v[0]] }; const float tz = (p[0]->z + p[1]->z + p[2]->z) / 3.0f;
                    if (tz < nd.z0 - 96.0f || tz > nd.z0 + 96.0f) continue; bool hit = false;
                    for (int e = 0; e < 3 && !hit; ++e) { const float ex = p[e + 1]->x - p[e]->x, ey = p[e + 1]->y - p[e]->y, dxl = nd.bx - nd.ax, dyl = nd.by - nd.ay, den = cr(dxl, dyl, ex, ey); if (std::fabs(den) < 1e-9f) continue;
                        const float t = cr(p[e]->x - nd.ax, p[e]->y - nd.ay, ex, ey) / den, u = cr(p[e]->x - nd.ax, p[e]->y - nd.ay, dxl, dyl) / den; hit = t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f; }
                    if (!hit) { const float s = cr(p[1]->x - p[0]->x, p[1]->y - p[0]->y, p[2]->x - p[0]->x, p[2]->y - p[0]->y) >= 0 ? 1.0f : -1.0f; hit = true; for (int e = 0; e < 3; ++e) if (s * cr(p[e + 1]->x - p[e]->x, p[e + 1]->y - p[e]->y, cxm - p[e]->x, cym - p[e]->y) < 0) hit = false; }   // the whole leaf inside one triangle
                    if (hit) { nd.tri = static_cast<std::int32_t>(g.triItems[k]); T.door = static_cast<std::int32_t>(E->doors0.size()); E->doors0.push_back(nd); ++E->plainDoors; } } } } }
        // components over what a PLANNER could ever join (the plan mesh when there are defects, so a defect never looks like an island)
        EQ::LabelComponents(E->planTris.empty() ? E->tris : E->planTris, E->walk, E->weak);
        { const EQ::View v = E->View(E->doors0, 0); EQ::BuildWalls(E->walls, v); EQ::IndexWalls(E->grid, E->walls); }
        E->triArea.resize(nt);
        for (std::size_t i = 0; i < nt; ++i) { const EQ::Tri& T = E->tris[i]; const EQ::Vec3& a = E->verts[T.v[0]]; const EQ::Vec3& b = E->verts[T.v[1]]; const EQ::Vec3& c = E->verts[T.v[2]];
            E->triArea[i] = 0.5f * std::fabs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)); if (E->walk[i] == 0) { E->area0 += E->triArea[i]; ++E->tris0; } }
        const EQ::View v = E->View(E->doors0, 0);
        if (const char* bad = EQ::Invalid(v)) { if (err) *err = bad; return nullptr; }   // the queries index without bounds checks: a view is checked once, here
        for (std::uint32_t i = 0; i < f.refCount(); ++i) { const mfoenv::Ref& r = f.ref(i); if (r.flags & mfoenv::kStartsDisabled) continue;
            const bool entry = r.kind == mfoenv::kDoorRef && (r.flags & mfoenv::kTeleport), actor = r.kind == mfoenv::kActor, loot = r.kind == mfoenv::kContainer || r.kind == mfoenv::kItem;
            if (!entry && !actor && !loot) continue;
            EnvPoint p; p.base = r.base; p.kind = r.kind; p.tri = EQ::NearestTri(v, { r.pos[0], r.pos[1], r.pos[2] }, entry ? 300.0f : 200.0f, p.p); if (p.tri < 0) continue;
            (entry ? E->entries : actor ? E->spawns : E->refs).push_back(p); }
        return E;
    }
    // A place a fight can be put in: a largest walk component with room for two groups.
    [[nodiscard]] inline const char* EnvUnusable(const SimEnv& E) {
        if (E.tris0 < 8) return "largest walk component under 8 triangles";
        if (E.area0 < 250000.0f) return "largest walk component under 250000 square units";
        return nullptr;
    }

    // THE POOL of this run: the fixtures (default), one file (--env), or a set of the extracted library (--env-set [--env-class]).
    struct EnvPool {
        std::vector<std::shared_ptr<SimEnv>> all;     // everything loaded, scenario-only fixtures included
        std::vector<const SimEnv*> draw;              // what an ordinary fight draws from
        std::vector<EnvSkip> skipped; std::string label = "fixtures";
        // THE LIBRARY HASH of this run (review F13): over the files an ordinary fight draws from, in name order. In the headline, the baseline's meta line
        // and the results file, so a compare across two libraries is refused like a compare across two worlds.
        [[nodiscard]] std::uint64_t Hash() const { std::vector<std::pair<std::string, std::uint64_t>> v; for (const SimEnv* e : draw) v.push_back({ e->name, e->hash }); std::sort(v.begin(), v.end());
            std::uint64_t h = 0x9E3779B97F4A7C15ULL; for (const auto& kv : v) h = SimEnv::Mix(h ^ kv.second); return h; }
        [[nodiscard]] const SimEnv* ByName(const std::string& n) const { for (const auto& e : all) if (e->name == n) return e.get(); return nullptr; }
        bool Add(const std::string& path, bool toDraw) { std::string err; auto E = LoadEnv(path, &err); if (!E) { skipped.push_back({ path, err }); return false; }
            if (const char* why = EnvUnusable(*E)) { skipped.push_back({ path, why }); return false; }
            all.push_back(E); if (toDraw && E->pool) draw.push_back(E.get()); return true; }
    };
    inline EnvPool g_envPool;
    inline thread_local const std::vector<const SimEnv*>* tl_envDraw = nullptr;   // --path-report: the places of ONE environment class, for the fights this thread builds
    inline std::string g_envDir =
#ifdef SIM_ENV_DIR
        SIM_ENV_DIR;
#else
        "tools/iaus_sim/envs";
#endif
    inline constexpr const char* kFixtureNames[] = { "open-room", "corridor-l", "doorway", "doorway-open", "doorway-plain", "locked-door", "stair-tower", "ledge", "ridge", "u-corridor", "dead-end", "island", "pillars", "narrow", "seam", "trust-wall", "trust-one-way" };
    inline bool LoadFixtures(EnvPool& P) { bool ok = true; for (const char* n : kFixtureNames) ok = P.Add(g_envDir + "/" + n + ".mfoenv", true) && ok; return ok; }
    // index.json of the library: every environment of a set (optionally one class). Returns the number loaded.
    inline int LoadSet(EnvPool& P, const std::string& set, const std::string& cls) {
        const char* home = std::getenv("HOME"); const std::string root = std::string(std::getenv("MFO_ENV_CACHE") ? std::getenv("MFO_ENV_CACHE") : (std::string(home ? home : ".") + "/.cache/mfo-env"));
        std::ifstream in(root + "/index.json"); if (!in) return -1; std::stringstream ss; ss << in.rdbuf(); const std::string s = ss.str(); JP jp{ s }; const J j = jp.val();
        int n = 0; for (const auto& kv : j["sets"].o) { if (set != "all" && kv.first != set) continue;
            for (const J& e : kv.second["envs"].a) { if (!cls.empty() && e["class"].s != cls) continue; if (P.Add(root + "/" + e["file"].s, true)) ++n; } }
        return n;
    }

}
