#pragma once
// iaus_sim/EnvTest.h -- SIM V4: known-answer checks of the environment layer on the hand-made fixtures (`sim --env-test`). HOST-ONLY.
// Each check names what it proves. They need no game data. Nothing here is a fight: bodies are walked by hand so an answer is exact.
#include "iaus_sim/Setup.h"

namespace Sim {

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
          g_v4 = keep; }
        std::printf("ENV-TESTS %s: %d checks, %d failed  (fixtures %zu loaded, %zu skipped)\n", fail ? "FAIL" : "PASS", n, fail, g_envPool.all.size(), g_envPool.skipped.size());
        return fail ? 1 : 0;
    }

}
