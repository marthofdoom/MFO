#pragma once
// iaus_sim/EnvTest.h -- SIM V4: known-answer checks of the environment layer on the hand-made fixtures (`sim --env-test`). HOST-ONLY.
// Each check names what it proves. They need no game data. Nothing here is a fight: bodies are walked by hand so an answer is exact.
#include "iaus_sim/Setup.h"

namespace Sim {

    // A place made in code: a lower floor (z 0, four squares along x), an upper floor EXACTLY over it (z 300), joined at the side by a flat square, a
    // two-square ramp and a landing. The file format's hand fixtures are height fields and cannot stack. Squares are 128 units.
    inline std::shared_ptr<SimEnv> MakeStacked() {
        auto E = std::make_shared<SimEnv>(); E->name = "stacked"; E->cls = "fixture_stacked"; E->pool = false; const float c = 128.0f;
        struct Sq { float x, y, zw, ze; };   // the square at (x, y), its height at the west and at the east side
        std::vector<Sq> sq; for (int i = 0; i < 4; ++i) sq.push_back({ static_cast<float>(i), 0, 0, 0 });
        sq.push_back({ 3, 1, 0, 0 }); sq.push_back({ 2, 1, 150, 0 }); sq.push_back({ 1, 1, 300, 150 }); sq.push_back({ 0, 1, 300, 300 });
        for (int i = 0; i < 4; ++i) sq.push_back({ static_cast<float>(i), 0, 300, 300 });
        for (const Sq& q : sq) { const std::uint32_t b = static_cast<std::uint32_t>(E->verts.size());
            E->verts.push_back({ q.x * c, q.y * c, q.zw }); E->verts.push_back({ (q.x + 1) * c, q.y * c, q.ze }); E->verts.push_back({ (q.x + 1) * c, (q.y + 1) * c, q.ze }); E->verts.push_back({ q.x * c, (q.y + 1) * c, q.zw });
            EQ::Tri a; a.v[0] = b; a.v[1] = b + 1; a.v[2] = b + 2; EQ::Tri d; d.v[0] = b; d.v[1] = b + 2; d.v[2] = b + 3; E->tris.push_back(a); E->tris.push_back(d); }
        auto same = [&](std::uint32_t p, std::uint32_t q) { const EQ::Vec3& a = E->verts[p]; const EQ::Vec3& b = E->verts[q]; return std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f && std::fabs(a.z - b.z) < 0.01f; };
        for (std::size_t i = 0; i < E->tris.size(); ++i) for (int k = 0; k < 3; ++k) for (std::size_t j = 0; j < E->tris.size(); ++j) { if (i == j) continue; for (int m = 0; m < 3; ++m)
            if (same(E->tris[i].v[k], E->tris[j].v[(m + 1) % 3]) && same(E->tris[i].v[(k + 1) % 3], E->tris[j].v[m])) { E->tris[i].nb[k] = static_cast<std::int32_t>(j); E->tris[i].kind[k] = EQ::Edge::Walk; } }
        EQ::BuildGrid(E->grid, E->verts, E->tris); EQ::LabelComponents(E->tris, E->walk, E->weak);
        { const EQ::View v = E->View(E->doors0, 0); EQ::BuildWalls(E->walls, v); EQ::IndexWalls(E->grid, E->walls); }
        E->triArea.assign(E->tris.size(), 0.5f * c * c); E->tris0 = static_cast<std::uint32_t>(E->tris.size()); E->area0 = 0.5f * c * c * static_cast<float>(E->tris.size());
        return E;
    }

    // THE SOLO WALK (review F1): one body, nobody else, walked from one triangle of a place to another, over every place of the pool. A trip is WRONG
    // when a route exists and he does not end at the goal: within 40 units of it and within 32 of its HEIGHT. This is the check that found walks
    // ending under or over their goal. `sim --solo-walk N`: N trips a place. The exit code is the wrong share over a tenth of a percent.
    inline int RunSoloWalk(const Data& D, const std::vector<SimClass>& classes, int trips) {
        const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3); long total = 0, wrong = 0, noRoute = 0, bad = 0, wrongZ = 0; int shown = 0;
        EQ::Scratch sc; EQ::Path path; const V4 keep = g_v4; g_v4.Defaults();
        for (const SimEnv* E : g_envPool.draw) {
            FightSpec fs; fs.cls = &classes[0]; fs.env = E; fs.seed = 11; fs.foes.push_back({ 0, { 0.0f, 0.0f } }); Fight F = BuildFight(D, fs, t1);
            for (Body& b : F.b) if (&b != &F.b[0]) b.alive = false;
            Body& w = F.b[0]; const std::size_t nt = E->tris.size(); std::uint64_t hs = SimEnv::Mix(E->hash ^ 0x501050105010ULL);
            auto pick = [&]() { hs = SimEnv::Mix(hs); std::size_t i = hs % nt; while (E->walk[i] != 0) i = (i + 1) % nt; return static_cast<std::int32_t>(i); };
            for (int k = 0; k < trips; ++k) { const std::int32_t a = pick(), g = pick(); if (a == g) continue;
                F.doors = E->doors0; F.doorStamp = 0; const EQ::View v = E->View(F.doors, 0, true); const EQ::Vec3 pa = EQ::Centroid(v, a), pg = EQ::Centroid(v, g);
                EQ::PathQuery q; q.from = pa; q.to = pg; q.fromTri = a; q.toTri = g; q.who = WalkerOf(w); ++total;
                if (!EQ::FindPath(v, q, sc, path) || !path.reachable) { ++noRoute; continue; }
                w.c.x = pa.x; w.c.y = pa.y; w.c.z = pa.z; w.nav = Nav{}; w.nav.tri = a; const EnvPoint goal{ pg, g, 0, 0 }; const long bad0 = F.t.envBad;
                const int ticks = static_cast<int>(30.0f * (2.0f * path.length / std::max(50.0f, w.spec->run) + 12.0f));
                for (int tk = 0; tk < ticks && std::hypot(pg.x - w.c.x, pg.y - w.c.y, pg.z - w.c.z) > 20.0f; ++tk) { F.Move(w, pg.x, pg.y, w.spec->run, false, nullptr, &goal); EnvStep(F); F.now += TM::dt; }
                bad += F.t.envBad - bad0; const bool xy = std::hypot(pg.x - w.c.x, pg.y - w.c.y) <= 40.0f, zz = std::fabs(pg.z - w.c.z) <= 32.0f;
                if (!xy || !zz) { ++wrong; if (xy) ++wrongZ; if (shown++ < 8) std::printf("SOLO-WALK WRONG %s: triangle %d -> %d, route %.0f, ended %.0f away in x, y and %.0f in height, on triangle %d at (%.0f, %.0f, %.0f), %d of %d corners, %s\n", E->name.c_str(), a, g, static_cast<double>(path.length), static_cast<double>(std::hypot(pg.x - w.c.x, pg.y - w.c.y)), static_cast<double>(w.c.z - pg.z), w.nav.tri, w.c.x, w.c.y, w.c.z, int(w.nav.i), int(w.nav.n), F.now < w.nav.holdUntil ? "holding after a leg end" : w.nav.stuckNoted ? "stuck" : "walking"); } }
        }
        g_v4 = keep; const long walked = total - noRoute;
        std::printf("SOLO-WALK  places %zu  trips %ld  no route %ld  walked %ld  WRONG %ld (%.3f%%), of them over or under the goal %ld  off-mesh steps %ld  => %s\n", g_envPool.draw.size(), total, noRoute, walked, wrong, walked ? 100.0 * double(wrong) / double(walked) : 0.0, wrongZ, bad,
                    walked > 0 && double(wrong) <= 0.001 * double(walked) && bad == 0 ? "PASS" : "FAIL");
        return walked > 0 && double(wrong) <= 0.001 * double(walked) && bad == 0 ? 0 : 1;
    }

    inline int RunEnvTests(const Data& D, const std::vector<SimClass>& classes) {
        int fail = 0, n = 0; auto ok = [&](bool c, const char* what) { ++n; if (!c) { ++fail; std::printf("ENV-TEST FAILED: %s\n", what); } };
        auto env = [&](const char* nm) -> const SimEnv& { const SimEnv* e = g_envPool.ByName(nm); if (!e) { std::printf("ENV-TEST: fixture %s not loaded\n", nm); std::exit(2); } return *e; };
        EQ::Scratch sc; EQ::Path path; const EQ::Walker man; EQ::Walker dropper; dropper.drops = true; dropper.maxDrop = 600.0f; EQ::Walker beast; beast.opensDoors = false;
        auto route = [&](const SimEnv& E, const std::vector<EQ::Door>& doors, EQ::Vec3 a, EQ::Vec3 b, const EQ::Walker& w, bool plan = false) { const EQ::View v = E.View(doors, 0, plan); EQ::PathQuery q; q.from = a; q.to = b; q.who = w; q.fromTri = EQ::TriAt(v, a); q.toTri = EQ::TriAt(v, b); return EQ::FindPath(v, q, sc, path); };
        auto dist = [](EQ::Vec3 a, EQ::Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); };
        auto ray = [&](const SimEnv& E, const std::vector<EQ::Door>& doors, EQ::Vec3 a, EQ::Vec3 b, const EQ::SightTuning& st = EQ::kSightTuning) { const EQ::View v = E.View(doors, 0); a.z += 108.0f; b.z += 66.0f; return EQ::CastRay(v, a, b, sc, st); };
        // a floor point by map square (x from the left, y from the top), as envs/make_fixtures.py lays a map out
        auto at = [&](const SimEnv& E, float cell, int rows, float sx, float sy) { const EQ::View v = E.View(E.doors0, 0); EQ::Vec3 p{ (sx + 0.5f) * cell, (static_cast<float>(rows) - sy - 0.5f) * cell, 0.0f }; float best = 1e30f; EQ::Vec3 on = p;
            for (const float z : { 0.0f, 150.0f, 300.0f, 400.0f }) { const std::int32_t t = EQ::TriAt(v, { p.x, p.y, z }); if (t >= 0) { const float h = EQ::HeightAt(v, t, p.x, p.y); if (std::fabs(h - z) < best) { best = std::fabs(h - z); on = { p.x, p.y, h }; } } } return on; };

        { const SimEnv& E = env("open-room"); const EQ::Vec3 a = E.entries[0].p, b = E.spawns[0].p;   // SHORTEST PATH in the open: the straight line, one corner
          ok(route(E, E.doors0, a, b, man) && path.reachable && path.pts.size() == 1 && std::fabs(path.length - dist(a, b)) < 1.0f, "open room: the route is the straight line");
          const EQ::View v = E.View(E.doors0, 0); const EQ::Walk w = EQ::WalkLine(v, EQ::TriAt(v, a), a, { a.x - 5000.0f, a.y, a.z }, man);
          ok(!w.reached && w.hit == EQ::Edge::Wall && w.end.x > 127.0f && w.end.x < 130.0f, "open room: a straight walk stops at the wall, half a unit inside");
          ok(E.walk[static_cast<std::size_t>(E.entries[0].tri)] == 0 && E.defects == 0 && E.doors0.empty(), "open room: one component, the load door is an entry and not a gate"); }
        { const SimEnv& E = env("doorway-open"); const EQ::Vec3 a = at(E, 128, 9, 2, 1), b = at(E, 128, 9, 18, 1), door = at(E, 128, 9, 10, 4);   // FUNNEL: through the one-square gap, pulled taut
          const bool r = route(E, E.doors0, a, b, man); const float taut = dist(a, door) + dist(door, b);
          ok(r && path.reachable && std::fabs(path.length - taut) < 40.0f && path.length >= dist(a, b), "doorway: the route goes through the gap and is taut (within 40 units of the two straight lines)");
          bool inGap = false; for (const EQ::PathPoint& p : path.pts) if (std::fabs(p.p.x - door.x) < 70.0f && std::fabs(p.p.y - door.y) < 70.0f) inGap = true;
          ok(inGap && path.pts.size() <= 4, "doorway: a corner stands in the gap, and the funnel left at most four corners");
          ok(!ray(E, E.doors0, a, b).clear && ray(E, E.doors0, at(E, 128, 9, 5, 4), at(E, 128, 9, 16, 4)).clear, "doorway: the wall blocks sight, the gap does not"); }
        { const SimEnv& E = env("doorway"); const EQ::Vec3 a = at(E, 128, 9, 5, 4), b = at(E, 128, 9, 16, 4);   // CLOSED DOOR: a cost for a man, a block for a beast, a sight blocker until opened
          ok(E.doors0.size() == 2 && E.doors0[0].state == EQ::DoorState::Closed && E.doors0[0].id == E.doors0[1].id, "door: the fixture's door is two triangles with one id, closed");
          ok(route(E, E.doors0, a, b, man) && path.reachable, "door: a man's route passes the closed door");
          ok(route(E, E.doors0, a, b, beast) && !path.reachable && path.blocked && path.blockDoor >= 0 && path.pts.back().p.x < 10.0f * 128.0f + 1.0f, "door: a beast's route is BLOCKED and ends in front of the door");
          ok(!ray(E, E.doors0, a, b).clear && ray(E, E.doors0, a, b).by == EQ::Blocker::Door, "door: closed, it blocks sight");
          std::vector<EQ::Door> open = E.doors0; for (EQ::Door& d : open) d.state = EQ::DoorState::Open;
          ok(ray(E, open, a, b).clear && route(E, open, a, b, beast) && path.reachable, "door: open, sight and the beast pass"); }
        { const SimEnv& E = env("doorway-plain"); const EQ::Vec3 a = at(E, 128, 9, 5, 4), b = at(E, 128, 9, 16, 4);   // A PLAIN DOOR: no DOOR row in the file, joined by its box
          ok(E.plainDoors == 2 && E.doors0.size() == 2 && E.doors0[0].leaf && E.doors0[0].id == E.doors0[1].id, "plain door: the loader joins the reference to the two triangles its leaf crosses");
          ok(route(E, E.doors0, a, b, beast) && !path.reachable && path.blocked && route(E, E.doors0, a, b, man) && path.reachable, "plain door: a block for a beast, a cost for a man");
          ok(!ray(E, E.doors0, a, b).clear && ray(E, E.doors0, a, b).by == EQ::Blocker::Door && ray(E, E.doors0, at(E, 128, 9, 5, 4), at(E, 128, 9, 9.2f, 4.3f)).clear, "plain door: the leaf blocks sight, and only the leaf (a ray that stops short of it on the door's own triangle is clear)"); }
        { const SimEnv& E = env("locked-door"); ok(E.doors0.size() == 2 && E.doors0[0].state == EQ::DoorState::Locked && route(E, E.doors0, E.entries[0].p, E.spawns[0].p, man) && !path.reachable && path.blocked, "locked door: blocked for a man without the key"); }
        { const SimEnv& E = env("ledge"); const EQ::Vec3 up = E.spawns[0].p, down = E.entries[0].p;   // DROP ONE-WAY
          ok(up.z > 290.0f && down.z < 1.0f, "ledge: the spawn stands on the platform, the entry on the floor");
          ok(route(E, E.doors0, up, down, dropper) && path.reachable && path.drops == 1, "drop: down the link with the ability, one drop");
          bool pair = false; for (std::size_t i = 0; i + 1 < path.pts.size(); ++i) if (path.pts[i].kind == EQ::Corner::DropStart && path.pts[i + 1].kind == EQ::Corner::DropLand && path.pts[i].p.z > 290.0f && path.pts[i + 1].p.z < 1.0f) pair = true;
          ok(pair, "drop: the route carries the take-off and the landing, 300 units apart in height");
          ok(!(route(E, E.doors0, down, up, dropper) && path.reachable), "drop: never walked UP (link kind 1 is not an edge)");
          ok(!(route(E, E.doors0, up, down, man) && path.reachable), "drop: not taken without the ability (the default)");
          ok(E.walk[static_cast<std::size_t>(E.spawns[0].tri)] != E.walk[static_cast<std::size_t>(E.entries[0].tri)] && E.weak[static_cast<std::size_t>(E.spawns[0].tri)] == E.weak[static_cast<std::size_t>(E.entries[0].tri)], "ledge: two walk components, one weak component");
          // SIGHT over a ledge: from the platform's rim he sees the far floor; from the foot of the cliff the platform's back is hidden; from the far floor its rim is seen
          ok(ray(E, E.doors0, at(E, 128, 8, 14.2f, 3), at(E, 128, 8, 4, 3)).clear, "ledge: from the rim he sees down to the far floor");
          ok(!ray(E, E.doors0, at(E, 128, 8, 12, 5), at(E, 128, 8, 17, 5)).clear, "ledge: from the foot of the cliff the back of the platform is hidden");
          ok(ray(E, E.doors0, at(E, 128, 8, 2, 5), at(E, 128, 8, 14.2f, 5)).clear, "ledge: from across the room the platform's rim is seen"); }
        { const SimEnv& E = env("island"); ok(!route(E, E.doors0, E.entries[0].p, E.spawns[0].p, dropper) && path.pts.empty(), "island: no route of any kind is refused at once");   // ISLAND REFUSAL
          ok(E.weak[static_cast<std::size_t>(E.entries[0].tri)] != E.weak[static_cast<std::size_t>(E.spawns[0].tri)], "island: two weak components"); }
        { const SimEnv& E = env("ridge"); const EQ::Vec3 a = E.entries[0].p, b = E.spawns[0].p, crest = at(E, 160, 9, 12.5f, 4);   // SIGHT OVER A RIDGE (terrain = the mesh's own triangles)
          ok(crest.z > 200.0f, "ridge: the crest is high");
          const EQ::Sight s = ray(E, E.doors0, a, b); ok(!s.clear && s.by == EQ::Blocker::Terrain, "ridge: the crest hides the far side");
          EQ::SightTuning noT = EQ::kSightTuning; noT.terrain = false; ok(ray(E, E.doors0, a, b, noT).clear, "ridge: with the terrain layer off the same ray is clear (the layer is what blocks)");
          ok(ray(E, E.doors0, crest, a).clear && ray(E, E.doors0, crest, b).clear, "ridge: from the crest both sides are seen");
          ok(ray(E, E.doors0, at(E, 160, 9, 2, 2), at(E, 160, 9, 7, 6)).clear, "ridge: two points on the same side see each other");
          ok(route(E, E.doors0, a, b, man) && path.reachable && path.climb > 200.0f && path.fall > 200.0f, "ridge: the route goes over it, and its climb and fall are counted"); }
        { const SimEnv& E = env("pillars");   // THE GAP RULE: a 48 unit pillar is furniture, the 288 unit block is a wall
          ok(ray(E, E.doors0, at(E, 48, 15, 10, 2), at(E, 48, 15, 10, 6)).clear, "pillars: a thin pillar does not block sight");
          ok(!ray(E, E.doors0, at(E, 48, 15, 22.5f, 5), at(E, 48, 15, 22.5f, 12)).clear, "pillars: the thick block does");
          ok(route(E, E.doors0, at(E, 48, 15, 22.5f, 5), at(E, 48, 15, 22.5f, 12), man) && path.reachable && path.length > 7.0f * 48.0f + 100.0f, "pillars: the route goes round the block"); }
        { const SimEnv& E = env("stair-tower"); const EQ::Vec3 top = at(E, 128, 11, 15.5f, 5), foot = at(E, 128, 11, 13.5f, 8);   // STAIRS: the real path, with its real length
          ok(top.z > 380.0f && foot.z < 1.0f, "tower: the platform is 384 up");
          ok(route(E, E.doors0, foot, top, man) && path.reachable && path.climb > 380.0f && path.length > 900.0f, "tower: the top is reached by the ramp, far longer than the straight line");
          ok(!ray(E, E.doors0, at(E, 128, 11, 14.5f, 7.2f), at(E, 128, 11, 15.5f, 5)).clear, "tower: from under the wall the top is hidden"); }
        { const SimEnv& E = env("seam"); const EQ::Vec3 a = E.entries[0].p, b = E.spawns[0].p;   // THE LOADER'S LINK KINDS
          ok(route(E, E.doors0, a, b, man) && path.reachable, "seam: kind 0 links join the two meshes");
          const EQ::Vec3 q = at(E, 128, 10, 3, 8); const EQ::View v = E.View(E.doors0, 0);
          ok(E.walk[static_cast<std::size_t>(EQ::TriAt(v, q))] != 0 && E.weak[static_cast<std::size_t>(EQ::TriAt(v, q))] == 0 && !(route(E, E.doors0, a, q, dropper) && path.reachable), "seam: the enable / disable portal is closed");
          int water = 0, pref = 0; for (const EQ::Tri& t : E.tris) { water += (t.flags & EQ::kTriWater) != 0; pref += (t.flags & EQ::kTriPreferred) != 0; } ok(water == 16 && pref == 28, "seam: the water and preferred-path flags are read"); }
        { const SimEnv& E = env("trust-wall"); const EQ::Vec3 a = at(E, 128, 8, 5, 2), b = at(E, 128, 8, 14, 2);   // DEFECT: the planner's mesh has an edge the truth lacks
          ok(E.defects == 2 && !E.planTris.empty(), "trust: the fixture carries its two defect rows");
          const bool pr = route(E, E.doors0, a, b, man, true); const float planLen = path.length; const bool tr = route(E, E.doors0, a, b, man, false);
          ok(pr && tr && path.reachable && planLen < 0.8f * path.length, "trust: the planner's route is the short one through the wall, the truth's is the long way round"); }
        { const SimEnv& E = env("corridor-l"); const SimEnv::Route r1 = E.FindRoute(E.doors0, 0, E.entries[0].tri, E.spawns[0].tri, man, true), r2 = E.FindRoute(E.doors0, 0, E.entries[0].tri, E.spawns[0].tri, man, true);   // THE ROUTE CACHE
          bool same = r1.n == r2.n && r1.reach && r2.reach; for (std::uint8_t i = 0; i < r1.n && same; ++i) same = r1.pts[i].p.x == r2.pts[i].p.x && r1.pts[i].p.y == r2.pts[i].p.y;
          ok(same && E.hits.load() >= 1, "cache: the second ask is a hit with the same corners");
          ok(!ray(E, E.doors0, E.entries[0].p, E.spawns[0].p).clear, "corridor: no sight round the corner"); }
        // BODIES. Walked by hand: three followers and the player from one room to the other through a corridor 64 units wide (two bodies need 60 each).
        { FightSpec fs; fs.cls = &classes[0]; fs.others = { &classes[0], &classes[0] }; fs.env = &env("narrow"); fs.envExact = true; fs.seed = 7; fs.foes.push_back({ 0, { 0.0f, 0.0f } });
          const V4 keep = g_v4; g_v4.Defaults(); const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3); Fight F = BuildFight(D, fs, t1); const SimEnv& E = *F.env;
          const EQ::Vec3 goal = at(E, 64, 9, 41, 4); int arrived = 0; bool bad = false; float nearest = 1e9f; const float c0 = 11.0f * 64.0f + 40.0f, c1 = 36.0f * 64.0f - 40.0f;
          for (Body& x : F.b) if (x.role == Role::Foe) x.alive = false;   // nobody fights: this is a walk
          for (int tick = 0; tick < 30 * 60; ++tick) {
              for (Body& x : F.b) { if (!x.Party()) continue; if (std::hypot(goal.x - x.c.x, goal.y - x.c.y) > 200.0f) F.Move(x, goal.x, goal.y, x.spec->run); if (!std::isfinite(x.c.x) || x.nav.tri < 0) bad = true; }
              for (std::size_t i = 0; i < F.b.size(); ++i) for (std::size_t j = i + 1; j < F.b.size(); ++j) { const Body& p = F.b[i]; const Body& q = F.b[j];
                  if (p.Party() && q.Party() && p.c.x > c0 && p.c.x < c1 && q.c.x > c0 && q.c.x < c1) nearest = std::min(nearest, std::hypot(p.c.x - q.c.x, p.c.y - q.c.y)); }
              EnvStep(F); F.now += TM::dt; }
          for (const Body& x : F.b) if (x.Party() && std::hypot(goal.x - x.c.x, goal.y - x.c.y) < 330.0f) ++arrived;
          ok(nearest >= 2.0f * g_envT.radius - 2.0f, "narrow corridor: nobody passes anybody (no two bodies ever closer than two radii inside it)"); ok(arrived == 4, "narrow corridor: all four are through within 60 s, in a queue"); ok(!bad && F.t.envBad == 0, "narrow corridor: nobody left the mesh");
          // and the closed door: a man opens it (door open time), the stamp changes, every route cache keys on it
          FightSpec f2 = fs; f2.env = &env("doorway"); Fight G = BuildFight(D, f2, t1); for (Body& x : G.b) if (x.role == Role::Foe) x.alive = false;
          const std::uint64_t st0 = G.doorStamp; Body& w = G.b[0]; const EQ::Vec3 g2 = at(*G.env, 128, 9, 17, 4); double openedAt = -1;
          for (int tick = 0; tick < 30 * 30; ++tick) { if (std::hypot(g2.x - w.c.x, g2.y - w.c.y) > 60.0f) G.Move(w, g2.x, g2.y, w.spec->run); if (openedAt < 0 && G.doors[0].state == EQ::DoorState::Open) openedAt = G.now; EnvStep(G); G.now += TM::dt; }
          ok(openedAt > 0 && G.doors[1].state == EQ::DoorState::Open && G.doorStamp != st0 && std::hypot(g2.x - w.c.x, g2.y - w.c.y) <= 61.0f, "closed door: he opens it (both triangles), the stamp changes, he arrives");
          // CAST STARTS (section 5). Today every row is seated (Harbinger ABI 46). --seats old: a claim on a row no seat started holds his hand for the short window, never fires, and is held off.
          { SimSpell par = Fx("TestParalyze", Prune::Nature::Control, kAvAlteration, -1, 10, 0, 0, 0); par.ctrlPar = true; par.row = kRowParalyze; par.e.form = 0x7E57; par.e.source = SpellSource::Spell;
            Fight C = BuildFight(D, fs, t1); Body& x = C.b[static_cast<std::size_t>(C.ctl[0].body)]; x.ctrlLive = true; std::uint32_t foe = 0; for (const Body& o : C.b) if (o.role == Role::Foe) foe = o.c.id;
            g_seatsOld = true; C.StartCast(x, 0, par, foe, 10.0f, true); g_seatsOld = false;
            ok(x.hitAt < 0 && std::fabs(x.actEnd - (C.now + 4.0)) < 1e-6 && C.t.castUnseated == 1 && C.t.castHeldOff == 1 && C.ctl[0].CastHeld(0x7E57, foe) && C.ctl[0].CastHeld(0x7E57, 12345u), "cast start, --seats old: a Paralyze claim never fires, stands 4 s (the short window) and is held off on every target");
            Fight C2 = BuildFight(D, fs, t1); Body& y = C2.b[static_cast<std::size_t>(C2.ctl[0].body)]; y.ctrlLive = true; C2.StartCast(y, 0, par, foe, 10.0f, true);
            ok(C2.t.castUnseated == 0 && !C2.ctl[0].CastHeld(0x7E57, 12345u), "cast start, today: the same claim goes down the measured road like any other (the row is seated)");
            ok(RowFightLong(kRowNone) && RowFightLong(kRowScript) && !RowFightLong(kRowSeated) && !RowFightLong(kRowParalyze) && !RowNeverStarts(kRowScript) && !RowNeverStarts(kRowNone), "cast start: a Script row is never refused, it takes the short window like a spell with no row; a seated row the long one");
            // ADOPT-SEATS MIRROR (native cast/SeatRows.h, WillWork.cpp (c) / (d), NoEffect.cpp): the seat table, the keyed will-work, the two-zero-fires hold
            ok(SimRowSeated(kRowParalyze, false) && SimRowSeated(kRowStagger, false) && SimRowSeated(kRowDisarm, false) && SimRowSeated(kRowTargetEffect, false) && SimRowSeated(kRowReanimate, false)
               && !SimRowSeated(kRowParalyze, true) && !SimRowSeated(kRowStagger, true) && !SimRowSeated(kRowDisarm, true) && !SimRowSeated(kRowTargetEffect, true) && !SimRowSeated(kRowReanimate, true)
               && SimRowSeated(kRowScript, true) && SimRowSeated(kRowSeated, true) && SimRowSeated(kRowNone, true), "seats: SeatRows::RowSeated serves the four hostile rows at ABI 46 and none of them under it; Reanimate only today; the other rows always");
            Fight Z = BuildFight(D, fs, t1); Body& zc = Z.b[static_cast<std::size_t>(Z.ctl[0].body)]; Body* zf = nullptr; for (Body& o : Z.b) if (o.role == Role::Foe) zf = &o;
            const auto imm = std::make_shared<ActorSpec>(*zf->spec); imm->immunePar = true; zf->spec = imm; const Ctl& zk = Z.ctl[0]; const std::uint32_t zid = zf->c.id;
            SimSpell p2 = par; p2.ctrl = true; p2.ctrlDur = 10.0f; SimSpell p3 = p2; p3.e.form = 0x7E58; p3.dmg = 5.0f; SimSpell p4 = p2; p4.e.form = 0x7E59;
            g_seatImmunitySeen = true; const bool seenNo = Z.WillWorkFor(zk, p2, *zf) == Tri::No, seenDmg = Z.WillWorkFor(zk, p3, *zf) == Tri::Yes; g_seatImmunitySeen = false;
            ok(Z.WillWork(p2, *zf) == Tri::No && Z.WillWorkFor(zk, p2, *zf) == Tri::Yes && seenNo, "will-work: paralysis immunity is the truth's No, the game's reader cannot see it (Yes), and --seat-immunity seen makes it a No");
            ok(seenDmg, "will-work: a keyed effect that cannot land is not a No while the spell carries another hostile damage effect");
            Z.SpellHit(zc, p2, *zf, true); const bool one = !zk.ZeroHeld(0x7E57, zid) && Z.t.castZeroFires == 1 && Z.t.castZeroHeld == 0;
            Z.SpellHit(zc, p2, *zf, true);
            ok(one && zk.ZeroHeld(0x7E57, zid) && Z.t.castZeroFires == 2 && Z.t.castZeroHeld == 1 && Z.WillWorkFor(zk, p2, *zf) == Tri::No && !zk.ZeroHeld(0x7E57, zid + 1), "zero effect: one fire that lands nothing holds nothing, the second holds the pair for the fight (and only that pair), and will-work then answers No");
            Z.SpellHit(zc, p3, *zf, true); Z.SpellHit(zc, p3, *zf, true); Z.SpellHit(zc, p3, *zf, true);
            ok(!zk.ZeroHeld(0x7E58, zid) && Z.t.castZeroFires == 2, "zero effect: a spell that also carries instant damage is never read (inconclusive), however often it fires");
            Z.SpellHit(zc, p4, *zf, true); imm->immunePar = false; Z.SpellHit(zc, p4, *zf, true); imm->immunePar = true; Z.SpellHit(zc, p4, *zf, true);
            ok(!zk.ZeroHeld(0x7E59, zid) && Z.t.castZeroFires == 4, "zero effect: a fire that lands clears the count (zero, landed, zero holds nothing)");
            g_seatsOld = true; Fight Z2 = BuildFight(D, fs, t1); Body& zc2 = Z2.b[static_cast<std::size_t>(Z2.ctl[0].body)]; Body* zf2 = nullptr; for (Body& o : Z2.b) if (o.role == Role::Foe) zf2 = &o; zf2->spec = imm;
            Z2.SpellHit(zc2, p2, *zf2, true); Z2.SpellHit(zc2, p2, *zf2, true); const bool oldNo = Z2.WillWorkFor(Z2.ctl[0], p2, *zf2) == Tri::No && Z2.t.castZeroFires == 0; g_seatsOld = false;
            ok(oldNo, "zero effect: --seats old is the game before it (no reading, will-work is the old answer)"); }
          // THE STUCK WINDOW, HOLDS AND REPEATS (review F8)
          { auto solo = [&](const SimEnv& E) { FightSpec f4 = fs; f4.env = &E; f4.envExact = false; Fight H = BuildFight(D, f4, t1); for (Body& b2 : H.b) b2.alive = false; H.b[static_cast<std::size_t>(H.ctl[0].body)].alive = true; return H; };
            auto place = [&](Fight& H, const EQ::Vec3& p) -> Body& { Body& s = H.b[static_cast<std::size_t>(H.ctl[0].body)]; const EQ::View v = H.env->View(H.doors, 0); s.c.x = p.x; s.c.y = p.y; s.c.z = p.z; s.nav = Nav{}; s.nav.tri = EQ::TriAt(v, p); return s; };
            const SimEnv& O = env("open-room"); const EQ::Vec3 a = O.entries[0].p, b = O.spawns[0].p;
            // a walker who is asked to move and cannot (speed 0), in bursts of three steps with a third of a second between them
            auto burst = [&](Fight& H, Body& s, int on, int off, double sec) { const EQ::View v = H.env->View(H.doors, 0); const EnvPoint g{ b, EQ::TriAt(v, b), 0, 0 }; int k = 0; for (const double end = H.now + sec; H.now < end; ++k) { if (k % (on + off) < on) H.Move(s, b.x, b.y, 0.0f, false, nullptr, &g); EnvStep(H); H.now += TM::dt; } };
            Fight J = solo(O); Body& sj = place(J, a); burst(J, sj, 3, 10, 15.0);
            ok(J.t.pathStuckSec > 0.5 && J.t.pathStuckLegs == 1 && J.t.pathRepeat == 0, "stuck: asking in bursts with a third of a second between them is stuck all the same (the window counts wanted seconds; a gap of three steps used to clear it)");
            for (int k = 0; k < 150; ++k) { EnvStep(J); J.now += TM::dt; } burst(J, sj, 1, 0, 3.5);
            ok(J.t.pathStuckLegs == 2 && J.t.pathRepeat == 1, "stuck: after standing five seconds by choice the window begins again, and sticking at the same spot again is a REPEAT");
            burst(J, sj, 1, 0, 4.2);
            ok(J.t.pathStuckLegs == 4 && J.t.pathRepeat == 3, "stuck: while he stays stuck the leg ends STUCK again every stuckSec (a route from where he stands each time), each one a repeat");
            Fight K = solo(O); Body& sk = place(K, a); burst(K, sk, 3, 100, 60.0);
            ok(K.t.pathStuckSec == 0.0 && K.t.pathStuckLegs == 0, "stuck: three steps of asking every 3.4 s is standing by choice (past wantGapSec), never stuck");
            const SimEnv& I = env("island"); const EQ::Vec3 ia = at(I, 128, 6, 2, 2), ib = at(I, 128, 6, 15, 2); Fight L = solo(I); Body& sl = place(L, ia); const EQ::View lv = L.env->View(L.doors, 0); const EnvPoint lg{ ib, EQ::TriAt(lv, ib), 0, 0 };
            for (int k = 0; k < 15 * 30; ++k) { L.Move(sl, ib.x, ib.y, sl.spec->run, false, nullptr, &lg); EnvStep(L); L.now += TM::dt; }
            ok(lg.tri >= 0 && L.t.pathUnreach >= 4 && L.t.pathRepeat >= 3 && L.t.pathHoldSec > 5.0 && L.t.pathStuckSec == 0.0 && L.t.envStuckForever == 0, "hold: a goal no route reaches is asked for again and again: every wait is HOLD time and none of it stuck time, and every end beside the last is a REPEAT");;
            // THE SIGHT DWELL AND THE BACK-OFF (re-check R1)
            Fight M = solo(O); Body& sm = place(M, a); Body* mf = nullptr; for (Body& o : M.b) if (o.role == Role::Foe) mf = &o;
            auto sw = [&](bool los, double sec) { bool r = false; for (const double end = M.now + sec; M.now < end; M.now += TM::dt) r = M.SightWalk(sm, *mf, los); return r; };
            const bool w1 = !sw(false, 1.4) && sw(false, 0.2), w2 = sw(true, 1.4) && !sw(true, 0.2); sw(false, 1.0); sw(true, 0.04); const bool w3 = !sw(false, 1.4) && sw(false, 0.2);
            ok(w1 && w2 && w3, "sight dwell: the walk-in begins after 1.5 s of no sight and ends after 1.5 s of sight, and one opposite sample starts the clock again (native kSightDwellMs)");
            { const bool fb = M.StyleOf(sm).fallback > 0.05f; const bool b0 = M.BackOff(sm, 300.0f, 320.0f, false), b1 = M.BackOff(sm, 300.0f, 320.0f, true), b2 = M.BackOff(sm, 400.0f, 320.0f, true), b3 = M.BackOff(sm, 430.0f, 320.0f, true), b4 = M.BackOff(sm, 400.0f, 320.0f, true);
              ok(!b0 && b1 == fb && b2 == fb && !b3 && !b4, "back-off: it begins inside its distance with sight, goes on until backOutU past it, does not begin again on the way out, and a style with no fallback never does it"); }
            // A SHUT LEAF INSIDE ONE TRIANGLE (re-check): a goal just past a door's leaf, in the leaf's own triangle, is not reached by a walker who cannot open it
            { const SimEnv& PD = env("doorway-plain"); const EQ::View pv = PD.View(PD.doors0, 0); int cases = 0, good = 0, same = 0;
              for (const EQ::Door& dr : PD.doors0) { if (!dr.leaf || dr.tri < 0) continue; const float ex = dr.bx - dr.ax, ey = dr.by - dr.ay, el = std::max(1e-3f, std::hypot(ex, ey));
                  for (const float fr : { 0.15f, 0.5f, 0.85f }) for (const float dl : { 3.0f, 6.0f, 10.0f, -3.0f, -6.0f, -10.0f }) { const float mx = dr.ax + ex * fr, my = dr.ay + ey * fr; EQ::Vec3 nearP{ mx + ey / el * dl, my - ex / el * dl, 0.0f }, farP{ mx - ey / el * dl, my + ex / el * dl, 0.0f };
                      nearP.z = EQ::HeightAt(pv, dr.tri, nearP.x, nearP.y); farP.z = EQ::HeightAt(pv, dr.tri, farP.x, farP.y);
                      if (EQ::TriAt(pv, farP) != dr.tri) continue;
                      const bool sameTri = EQ::TriAt(pv, nearP) == dr.tri; const EQ::Vec3 from = sameTri ? nearP : PD.entries[0].p;
                      if (!sameTri && !(route(PD, PD.doors0, from, nearP, beast) && path.reachable)) continue;   // the far point must be the far side as seen from the entry
                      ++cases; same += sameTri;
                      const bool rb = route(PD, PD.doors0, from, farP, beast); const bool beastStops = rb && !path.reachable && path.blocked && path.blockDoor >= 0 && PD.doors0[static_cast<std::size_t>(path.blockDoor)].id == dr.id;
                      const bool rm = route(PD, PD.doors0, from, farP, man); good += beastStops && rm && path.reachable; } }
              ok(cases > 0 && good == cases && same > 0, "leaf: a goal across a closed leaf inside the leaf's own triangle is BLOCKED for a beast (the path names the door) and reached by a man, from the same triangle and from the room"); }
            { const SimEnv& PD = env("doorway-plain"); EQ::View bv = PD.View(PD.doors0, 0); std::vector<EQ::Wall> ws(bv.walls.begin(), bv.walls.end()); std::vector<EQ::Door> ds(bv.doors.begin(), bv.doors.end());
              const bool clean = EQ::Invalid(bv) == nullptr && !ws.empty() && !ds.empty(); ws[0].hi = std::numeric_limits<float>::quiet_NaN(); EQ::View v1 = bv; v1.walls = ws;
              bool leafBad = false; for (EQ::Door& d : ds) if (d.leaf) { d.bx = std::numeric_limits<float>::infinity(); leafBad = true; break; } EQ::View v2 = bv; v2.doors = ds;
              ok(clean && EQ::Invalid(v1) != nullptr && leafBad && EQ::Invalid(v2) != nullptr, "snapshot check: a wall or a door leaf that is not finite is refused"); }
            // THE APPROACH CLAIM'S BOUNDS AT A DOOR HE CANNOT OPEN (re-check R2)
            { const SimEnv& LK = env("locked-door"); auto setup = [&](Fight& H, bool caster) -> Body& { Body& s = place(H, LK.entries[0].p); Body* f = nullptr; for (Body& o : H.b) if (o.role == Role::Foe) f = &o;
                  f->alive = true; const EQ::View v = H.env->View(H.doors, 0); const EQ::Vec3 p = LK.spawns[0].p; f->c.x = p.x; f->c.y = p.y; f->c.z = p.z; f->nav = Nav{}; f->nav.tri = EQ::TriAt(v, p); s.casterMode = caster; s.rangedMode = false; return *f; };
              auto walk = [&](Fight& H, Body& f, double sec) { Body& s = H.b[static_cast<std::size_t>(H.ctl[0].body)]; for (const double end = H.now + sec; H.now < end; H.now += TM::dt) { H.Move(s, f.c.x, f.c.y, s.spec->run, false, &f); EnvStep(H); } };
              Fight C1 = solo(LK); Body& f1 = setup(C1, true); Body& s1 = C1.b[static_cast<std::size_t>(C1.ctl[0].body)]; walk(C1, f1, 45.0);
              const bool capped = C1.t.apprDrops == 3 && C1.t.apprBlocked == 1 && EnvApprBlocked(C1, s1, f1.c.id); const long legs0 = C1.t.pathLegs; walk(C1, f1, 10.0); const bool quiet = C1.t.pathLegs == legs0;
              ok(capped && quiet, "door block: a caster's approach on a foe behind a door he cannot open is backed off three times (offenseApproachDropCap), then blocked, and he does not walk at it again");
              place(C1, { s1.c.x - 400.0f, s1.c.y, s1.c.z }); s1.casterMode = true; const bool tri = s1.nav.tri >= 0, lapsed = tri && !EnvApprBlocked(C1, s1, f1.c.id); walk(C1, f1, 12.0);
              ok(lapsed && C1.t.pathLegs > legs0 && C1.t.apprBlocked == 2, "door block: it lapses once he stands blockLapseU from where it was made, the walk is filed again, and the next undone path blocks at once (the count stands)");
              Fight C2 = solo(LK); Body& f2 = setup(C2, false); walk(C2, f2, 45.0);
              ok(C2.t.apprDrops == 0 && C2.t.apprBlocked == 0 && C2.t.pathBlocked >= 8 && C2.t.pathHoldSec > 10.0, "door block: a blade's pursuit is never bound by the claim (no backoff, no cap): he asks again every retry and it is counted as hold time and re-blocks"); } }
          // STACKED FLOORS (review F1, F2, F6): an upper floor exactly over a lower one, joined by a ramp at the side. The same x, y exists on both.
          { const std::shared_ptr<SimEnv> SE = MakeStacked(); const SimEnv& S2 = *SE; const EQ::View v = S2.View(S2.doors0, 0); const EQ::Vec3 lo{ 200.0f, 40.0f, 0.0f }, up{ 200.0f, 40.0f, 300.0f };
            const std::int32_t tl = EQ::TriAt(v, lo), tu = EQ::TriAt(v, up); const EQ::Walk w = EQ::WalkLine(v, tl, lo, up, man);
            ok(tl >= 0 && tu >= 0 && tl != tu && EQ::Invalid(v) == nullptr, "stacked: the point has a triangle on each floor, and the view passes the snapshot check");
            ok(w.reached && !w.At() && std::fabs(w.zMiss + 300.0f) < 1.0f, "stacked: a straight walk 'reaches' the x, y and reports it is 300 under the goal (WalkLine::At is false)");
            ok(route(S2, S2.doors0, lo, up, man) && path.reachable && path.length > 600.0f && path.climb > 290.0f, "stacked: the route to the floor above goes round by the ramp");
            FightSpec f3 = fs; f3.env = &S2; f3.envExact = false; Fight H = BuildFight(D, f3, t1); for (Body& b2 : H.b) if (b2.role == Role::Foe || b2.role == Role::Follower) b2.alive = false;
            Body& wk = H.b[0]; auto put = [&](Body& b2, const EQ::Vec3& p, std::int32_t t) { b2.c.x = p.x; b2.c.y = p.y; b2.c.z = p.z; b2.nav = Nav{}; b2.nav.tri = t; };
            put(wk, lo, tl); const EnvPoint goal{ up, tu, 0, 0 };
            for (int tick = 0; tick < 30 * 30; ++tick) { if (std::hypot(up.x - wk.c.x, up.y - wk.c.y, up.z - wk.c.z) > 30.0f) H.Move(wk, up.x, up.y, wk.spec->run, false, nullptr, &goal); EnvStep(H); H.now += TM::dt; }
            ok(std::fabs(wk.c.z - 300.0f) < 24.0f && std::hypot(up.x - wk.c.x, up.y - wk.c.y) < 40.0f, "stacked: a walk to a point of the floor above ENDS on the floor above, not under it");
            Body& st = H.b[static_cast<std::size_t>(H.ctl[0].body)]; st.alive = true; put(st, up, tu); put(wk, lo, tl);   // and to a BODY on the floor above: his storey is the goal's
            for (int tick = 0; tick < 30 * 30; ++tick) { if (Fight::Dist(wk, st) > 90.0f) H.Move(wk, st.c.x, st.c.y, wk.spec->run, false, &st); EnvStep(H); H.now += TM::dt; }
            ok(std::fabs(wk.c.z - 300.0f) < 24.0f && Fight::Dist(wk, st) < 120.0f, "stacked: a walk to a body on the floor above ends beside him");
            put(wk, lo, tl);
            ok(!EnvMeleeClear(H, wk, st), "stacked: no blow through the floor");
            ok(!EnvInBlast(H, lo.x, lo.y, lo.z, 400.0f, st), "stacked: a blast on the floor below does not burn through the floor");
            ok(EnvInBlast(H, up.x + 100.0f, up.y, up.z, 400.0f, st), "stacked: a blast on his own floor reaches him");
            ok(!H.Sees(wk, st), "stacked: no sight through the floor");
            ok(H.t.envBad == 0, "stacked: nobody left the mesh"); }
          g_v4 = keep; }
        // ── BUNDLE A (iaus-sim-v4.md 16): summons as bodies, hazards ──
        { const V4 keep = g_v4; g_v4.Defaults(); const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3); const SimClass* conj = &classes[0]; for (const SimClass& c : classes) if (c.name == "conjurer") conj = &c;
          FightSpec fs; fs.cls = conj; fs.env = &env("open-room"); fs.envExact = true; fs.seed = 21; fs.level = 20; fs.rank = 3; fs.ensure = { "summon" }; fs.foeDist = 900; for (int i = 0; i < 3; ++i) fs.foes.push_back({ 0, { 60.0f * static_cast<float>(i), 0.0f } });
          // a conjurer with TWO summon spells and magicka for both
          auto two = [&](Fight& F) { Ctl& c = F.ctl[0]; auto fb = std::make_shared<FollowerBuild>(*c.build); int si = -1; for (std::size_t i = 0; i < fb->spells.size(); ++i) if (fb->spells[i].summon) si = static_cast<int>(i); if (si < 0) return std::pair{ -1, -1 };
              SimSpell s = fb->spells[static_cast<std::size_t>(si)]; s.e.form = 0x5C0FF; s.e.name = "SecondSummon"; SpellEntry e = c.book.entries[static_cast<std::size_t>(si)]; e.form = s.e.form; e.name = s.e.name; fb->spells.push_back(s); fb->book.entries.push_back(e); c.build = fb; c.book.entries.push_back(e);
              c.active.resize(fb->spells.size()); c.summonUntil.resize(fb->spells.size(), 0.0); Body& x = F.b[static_cast<std::size_t>(c.body)]; x.c.mpMax = x.c.mp = 5000; return std::pair{ si, static_cast<int>(fb->spells.size()) - 1 }; };
          { Fight F = BuildFight(D, fs, t1); const auto [s1, s2] = two(F); const std::uint32_t me = F.b[static_cast<std::size_t>(F.ctl[0].body)].c.id; ok(s1 >= 0, "summons: the conjurer holds a summon spell");
            auto cast = [&](int si) { Body& x = *F.Find(me); x.actSpell = si; F.LandCommanded(x, Fight::GenericSummonSpec(20), x.c.x + 150.0f, x.c.y, F.now + 60.0); };
            auto live = [&]() { int n = 0; for (const Body& o : F.b) n += o.role == Role::Summon && o.alive; return n; };
            ok(F.SummonLimit(*F.Find(me)) == 1, "summons: the limit of a tier 2 conjurer is 1"); ok(F.SummonGate(*F.Find(me), s1) == Fight::kSummonClear, "summons: the gate is clear with nothing out");
            cast(s1); ok(live() == 1 && F.SummonGate(*F.Find(me), s1) == Fight::kSummonLive && F.SummonGate(*F.Find(me), s2) == Fight::kSummonAtLimit, "summons: the same spell reads Live, another reads the limit");
            const Body* q = nullptr; for (const Body& o : F.b) if (o.role == Role::Summon) q = &o; ok(q && q->nav.tri >= 0, "summons: the creature stands on the mesh");
            cast(s2); ok(live() == 1 && F.CommandedBy(me) == 1 && F.ctl[0].summonUntil[static_cast<std::size_t>(s1)] <= F.now, "summons: a landing past the limit dismisses the oldest (and its spell is free again)");
            for (Body& o : F.b) if (o.role == Role::Summon && o.alive) { o.alive = false; } F.CommandedStep(); ok(F.CommandedBy(me) == 1 && F.SummonGate(*F.Find(me), s1) == Fight::kSummonAtLimit && F.ctl[0].summonUntil[static_cast<std::size_t>(s2)] <= F.now, "summons: the dead one still counts against the limit, and its spell is no longer live");
            F.now += static_cast<double>(kSummon.deadListedSec) + 0.1; F.CommandedStep(); ok(F.SummonGate(*F.Find(me), s1) == Fight::kSummonClear, "summons: removed from the list, the gate is clear");
            cast(s1); F.now += 61.0; F.CommandedStep(); ok(live() == 0 && F.CommandedBy(me) == 0, "summons: its time is over, it goes"); 
            // a foe at its limit: the same eviction, on its side
            std::uint32_t fid = 0; for (const Body& o : F.b) if (o.role == Role::Foe) fid = o.c.id; auto foeCast = [&]() { Body& x = *F.Find(fid); x.actSpell = 0; F.LandCommanded(x, Fight::GenericSummonSpec(20), x.c.x, x.c.y + 100.0f, F.now + 60.0); };
            foeCast(); std::uint32_t first = F.b.back().c.id; foeCast(); ok(!F.Find(first)->alive && F.b.back().alive && F.b.back().role == Role::Foe && F.CommandedBy(fid) == 1, "summons: a foe's second summon dismisses its first"); }
          // whole fights: never two at once, and IAUS never casts the second while the first lives (the gate, not the eviction)
          { int most = 0, evicted = 0, landed = 0; for (std::uint64_t sd = 1; sd <= 4; ++sd) { fs.seed = sd; Fight F = BuildFight(D, fs, t1); (void)two(F); IausPolicy P; std::size_t bodies = F.b.size(); double lastLive = -1;
              while (!F.over && F.now < 60.0) { if (F.now >= F.nextPump) { F.nextPump += TM::pump; P.Serve(F, F.rr); F.rr = (F.rr + 1) % static_cast<int>(F.ctl.size()); } int before = 0; for (const Body& o : F.b) before += o.role == Role::Summon && o.alive; F.StepWorld(); int n = 0; for (const Body& o : F.b) n += o.role == Role::Summon && o.alive; most = std::max(most, n);
                  if (F.b.size() > bodies) { landed += static_cast<int>(F.b.size() - bodies); if (before > 0 && n > 0 && F.now - lastLive < 0.05) { for (const Commanded& e : F.commanded) if (e.deadAt < 0) ++evicted; } bodies = F.b.size(); } if (n > 0) lastLive = F.now; } }
            ok(landed >= 1 && most == 1, "summons: in whole IAUS fights there is never a second live creature of one conjurer"); ok(evicted == 0, "summons: IAUS never casts into the eviction"); }
          { fs.seed = 3; Fight F = BuildFight(D, fs, t1); (void)two(F); OraclePolicy O; O.cfg.K = 2; O.cfg.H = 3.0; O.cfg.every = 1.0; int most = 0; while (!F.over && F.now < 10.0) { if (F.now >= F.nextPump) { F.nextPump += TM::pump; O.Serve(F, F.rr); F.rr = (F.rr + 1) % static_cast<int>(F.ctl.size()); } F.StepWorld(); int n = 0; for (const Body& o : F.b) n += o.role == Role::Summon && o.alive; most = std::max(most, n); }
            ok(most <= 1, "summons: the oracle neither (engine-bound)"); }
          // its own AI: a creature with a bolt and no melee fights at range, and is a foe's target
          { fs.seed = 5; Fight F = BuildFight(D, fs, t1); for (Body& o : F.b) if (o.role == Role::Follower) o.alive = o.role != Role::Follower; auto sp = Fight::GenericSummonSpec(20); sp->spells = { 0 }; sp->mp = 400; sp->mpR = 20; sp->unarmed = 0.1f; F.ctl.clear();
            Body& n = F.Add(Role::Summon, sp, F.b[0].c.x + 150.0f, F.b[0].c.y); n.airUntil = -60.0; const std::uint32_t id = n.c.id; float far = 0; double dealt = 0; bool drew = false;
            for (int i = 0; i < 30 * 20 && !F.over; ++i) { F.StepWorld(); const Body* q = F.Find(id); if (!q || !q->alive) break; if (q->dmgDealt > dealt + 1.0) { dealt = q->dmgDealt; for (const Body& o : F.b) if (o.c.id == q->c.targetId) far = std::max(far, Fight::Dist(*q, o)); } for (const Body& o : F.b) drew = drew || (o.role == Role::Foe && o.c.targetId == id); }
            ok(dealt > 20.0 && far > 250.0f, "summons: a creature with a bolt deals its damage at range"); ok(drew, "summons: a foe goes for the summon"); }
          // a standing creature holds a one body corridor
          { FightSpec f2 = fs; f2.env = &env("narrow"); f2.foes.resize(1); Fight F = BuildFight(D, f2, t1); const SimEnv& E = *F.env; const EQ::Vec3 mid = at(E, 64, 9, 24, 4), from = at(E, 64, 9, 41, 4), to = at(E, 64, 9, 4, 4); auto sp = Fight::GenericSummonSpec(20);
            Body& n = F.Add(Role::Summon, sp, mid.x, mid.y); n.airUntil = -600.0; const float nx = n.c.x; float least = 1e9f; for (Body& o : F.b) if (o.role == Role::Player || o.role == Role::Follower) o.alive = false;
            for (Body& o : F.b) if (o.role == Role::Foe) { o.c.x = from.x; o.c.y = from.y; o.c.z = from.z; EnvAdopt(F, o); }
            for (int i = 0; i < 30 * 20; ++i) { for (Body& o : F.b) if (o.role == Role::Foe) { F.Move(o, to.x, to.y, o.spec->run); least = std::min(least, o.c.x - nx); } EnvStep(F); F.now += TM::dt; }
            ok(least > 0.0f && least < 400.0f, "summons: a foe walks up to a creature in a one body corridor and does not pass it"); }
          // HAZARDS
          { fs.seed = 9; Fight F = BuildFight(D, fs, t1); auto hz = std::make_shared<SimSpell>(Fx("FixWallHazard", Prune::Nature::Offense, kAvDestruction, kAvResistFire, 0, 6, 0, 0)); SimSpell wall = Fx("FixWall", Prune::Nature::Offense, kAvDestruction, kAvResistFire, 0, 8, 0, 0); wall.e.concentration = true;
            wall.hazard = HazardSpec{ 100.0f, 5.0f, 0.5f, 2, true, true, false, hz }; ok(Fight::LaysHazard(wall), "hazards: a wall lays one"); { SimSpell bolt = wall; bolt.e.concentration = false; SimSpell fl = wall; fl.hazard->drops = false; ok(!Fight::LaysHazard(bolt) && !Fight::LaysHazard(fl), "hazards: a bolt, and a stream whose hazard does not drop to the ground, lay none"); }
            Body* me = nullptr; std::vector<Body*> foes; for (Body& o : F.b) { if (o.role == Role::Follower) me = &o; if (o.role == Role::Foe) foes.push_back(&o); } Body& in = *foes[0]; Body& out = *foes[1]; foes[2]->alive = false; out.c.x = in.c.x + 250.0f; out.c.y = in.c.y; me->c.x = in.c.x + 20.0f; me->c.y = in.c.y; me->c.z = in.c.z; in.c.hp = in.c.hpMax = 4000; out.c.hp = out.c.hpMax = 4000;
            const float h0 = me->c.hp; F.SpawnHazard(*me, wall, in.c.x, in.c.y, in.c.z); F.SpawnHazard(*me, wall, in.c.x, in.c.y, in.c.z);   // two segments on one spot: one victim is hit once per interval
            auto run = [&](double sec) { for (const double e = F.now + sec; F.now < e; F.now += TM::dt) F.HazardStep(); };
            run(2.0); const float took = in.c.hpMax - in.c.hp; ok(took > 0.0f && out.c.hp == out.c.hpMax, "hazards: who stands in it takes the damage, who stands outside takes none"); ok(me->c.hp == h0, "hazards: the caster's party takes none");
            const float el = std::clamp(F.ResistOf(in, kAvResistFire) / 100.0f, -1.0f, 1.0f), mr = std::clamp(F.ResistOf(in, kAvResistMagic) / 100.0f, -1.0f, 1.0f), per = 6.0f * me->dmgScale * (1.0f - el) * (1.0f - mr); ok(std::fabs(took - 4.0f * per) < 1.01f * per, "hazards: one tick per interval whatever the number of segments");
            F.SpawnHazard(*me, wall, in.c.x, in.c.y, in.c.z); ok(F.hazards.size() == 2, "hazards: the record's limit removes the oldest");
            run(6.0); const float after = in.c.hp; run(3.0); ok(F.hazards.empty() && in.c.hp == after, "hazards: nothing after its lifetime"); }
          { const std::string js = "{\"edid\":\"T\",\"delivery\":\"Aimed\",\"castType\":\"Concentration\",\"cost\":10,\"effects\":[{\"mag\":8,\"area\":0,\"dur\":0,\"hostile\":true,\"detrimental\":true,\"archetype\":0,\"av\":24,\"resist\":41,\"baseCost\":1,\"skill\":\"Destruction\",\"hazard\":{\"edid\":\"H\",\"via\":\"impact\",\"limit\":10,\"radius\":3,\"lifetime\":30,\"interval\":0.3,\"flags\":20,\"spell\":{\"edid\":\"HS\",\"delivery\":\"Aimed\",\"castType\":\"FireAndForget\",\"cost\":0,\"effects\":[{\"mag\":20,\"area\":0,\"dur\":1,\"hostile\":true,\"detrimental\":true,\"archetype\":0,\"av\":24,\"resist\":41,\"baseCost\":0,\"skill\":\"Destruction\"}]}}}]}";
            JP jp{ js }; const J j = jp.val(); SimSpell s; Classify(j, s); ok(s.hazard && s.hazard->limit == 10 && std::fabs(s.hazard->radius - Prune::BlastUnits(3.0f, 0.0f, false)) < 1e-3f && std::fabs(s.hazard->spell->dmg - 6.0f) < 1e-3f && s.dmg == 16.0f && Fight::LaysHazard(s), "hazards: the reader (radius in feet, 20 a second at 0.3 s is 6 a tick, the spell's own damage untouched)"); }
          g_v4 = keep; }
        std::printf("ENV-TESTS %s: %d checks, %d failed  (fixtures %zu loaded, %zu skipped)\n", fail ? "FAIL" : "PASS", n, fail, g_envPool.all.size(), g_envPool.skipped.size());
        return fail ? 1 : 0;
    }

}
