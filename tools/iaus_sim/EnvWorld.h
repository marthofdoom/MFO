#pragma once
// iaus_sim/EnvWorld.h -- SIM V4: bodies on the mesh (Docs/research/2026-11/iaus-sim-v4.md 2.3, 2.4, 2.5). HOST-ONLY.
// Included at the end of World.h: the functions World.h's Fight calls when a fight has an environment (Fight::env).
// With no environment none of this runs and the lab is v3, draw for draw.
//
// WHAT IS MODELLED (each a MODEL with a knob in Env.h EnvTuning; the engine's own steering is not decoded)
//   EnvMove      every mover of World.h ends here. A SEEK walks a route (the planner's mesh through FindPath, cached per
//                triangle pair), an AWAY move (backpedal, flee) walks straight and slides along walls. The BODY always walks
//                the TRUE mesh, so a defect between the two is lived, as in the game (8.9).
//   doors        a closed door he can open costs EnvTuning::doorOpenSec at the door, then it is open for everyone.
//   bodies       circles. A step into another body is refused, he tries one side step round it, else he waits: a friend lets
//                him through after yieldSec, a foe never. No way round for blockSec = the leg ends BLOCKED.
//   leg ends     arrived / BLOCKED (a door he cannot open, a body) / UNREACHABLE (no route: he stands at the nearest point
//                the search reached) / STUCK (no progress while a leg wants him to move). After an end he waits retrySec.
//   drops        a Drop edge is taken only by a walker whose abilities say so (V4::drops / foedrops, default OFF).
//   EnvSees      three rays as native Sightline.cpp casts them: eye to feet, torso and head, each 48 units short of the body.
//
// THE SIX ODD-PATHING TALLIES (2.5) are counted here, for the subject: Tally::path*.
#include "iaus_sim/World.h"

namespace Sim {

    [[nodiscard]] inline float EnvRadius(const Body& x) { return x.spec->mass > g_envT.largeMass ? g_envT.radiusLarge : g_envT.radius; }
    [[nodiscard]] inline EQ::Walker WalkerOf(const Body& x) {
        EQ::Walker w; w.large = x.spec->mass > g_envT.largeMass; w.radius = EnvRadius(x);
        w.opensDoors = !(x.spec->animal || x.spec->creature || x.spec->dragon);   // MODEL: beasts do not open doors (the race flag is not in the extract)
        w.drops = x.Party() ? g_v4.drops : g_v4.foedrops; w.maxDrop = g_envT.maxDrop; return w;
    }
    [[nodiscard]] inline EQ::View EnvViewOf(const Fight& F, bool plan = false) { return F.env->View(F.doors, F.doorStamp, plan); }
    inline thread_local EQ::Scratch tl_envScratch;

    // Put a body on the mesh near where it is (a spawn, a summon, a landing). Height is taken from the nearest other body.
    inline void EnvAdopt(Fight& F, Body& x) {
        const EQ::View v = EnvViewOf(F); float zref = x.c.z, bd = 1e30f;
        for (const Body& o : F.b) if (&o != &x && o.nav.tri >= 0) { const float d = std::hypot(o.c.x - x.c.x, o.c.y - x.c.y); if (d < bd) { bd = d; zref = o.c.z; } }
        EQ::Vec3 on; std::int32_t t = EQ::NearestTri(v, { x.c.x, x.c.y, zref }, 4000.0f, on);
        if (t < 0 && !F.b.empty() && F.b[0].nav.tri >= 0) { t = F.b[0].nav.tri; on = { F.b[0].c.x, F.b[0].c.y, F.b[0].c.z }; }
        if (t < 0) return;
        x.c.x = on.x; x.c.y = on.y; x.c.z = on.z; x.nav = Nav{}; x.nav.tri = t;
    }

    inline bool EnvSeesPoint(const Fight& F, const EQ::Vec3& eye, const Body& o) {
        const EnvTuning& T = g_envT; const EQ::View v = EnvViewOf(F);
        for (const float h : { T.feetU, T.torsoFrac * T.bodyH, T.eyeFrac * T.bodyH }) {
            EQ::Vec3 to{ o.c.x, o.c.y, o.c.z + h }; const float dx = to.x - eye.x, dy = to.y - eye.y, dz = to.z - eye.z, len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len <= T.targetMargin) return true;
            const float k = (len - T.targetMargin) / len; to = { eye.x + dx * k, eye.y + dy * k, eye.z + dz * k };
            if (EQ::CastRay(v, eye, to, tl_envScratch, g_sightT).clear) return true;
        }
        return false;
    }
    inline bool EnvSees(const Fight& F, const Body& a, const Body& o) {
        if (!g_v4.los) return true;
        return EnvSeesPoint(F, { a.c.x, a.c.y, a.c.z + g_envT.eyeFrac * g_envT.bodyH }, o);
    }
    // Would `who` see o from the floor point (px, py) of his own level?
    inline bool EnvSeesFrom(const Fight& F, const Body& who, float px, float py, const Body& o) {
        if (!g_v4.los) return true;
        const EQ::View v = EnvViewOf(F); const std::int32_t t = EQ::TriAt(v, { px, py, who.c.z });
        const float z = t >= 0 ? EQ::HeightAt(v, t, px, py) : who.c.z;
        return EnvSeesPoint(F, { px, py, z + g_envT.eyeFrac * g_envT.bodyH }, o);
    }
    // Is (px, py) floor of his own level (within a storey of his height) with no closed door on it?
    inline bool EnvStandable(const Fight& F, const Body& who, float px, float py) {
        const EQ::View v = EnvViewOf(F); const std::int32_t t = EQ::TriAt(v, { px, py, who.c.z }); if (t < 0) return false;
        if (std::fabs(EQ::HeightAt(v, t, px, py) - who.c.z) > 150.0f) return false;
        const std::int32_t d = v.tris[static_cast<std::size_t>(t)].door; return d < 0 || F.doors[static_cast<std::size_t>(d)].state == EQ::DoorState::Open;
    }

    // Can a blow pass between them: the straight walk from one to the other is open floor (no wall, no closed door, no ledge), on one level.
    inline bool EnvMeleeClear(const Fight& F, const Body& a, const Body& o) {
        if (a.nav.tri < 0 || std::fabs(a.c.z - o.c.z) > 110.0f) return a.nav.tri < 0;
        const EQ::Walk w = EQ::WalkLine(EnvViewOf(F), a.nav.tri, { a.c.x, a.c.y, a.c.z }, { o.c.x, o.c.y, o.c.z }, EQ::Walker{});
        return w.reached || std::hypot(w.end.x - o.c.x, w.end.y - o.c.y) < 24.0f;
    }

    enum class LegEnd { Blocked, Unreachable, Stuck };
    inline void EnvLegEnd(Fight& F, Body& x, LegEnd how) {
        Nav& n = x.nav; const bool subj = x.ctl == 0;
        if (how != LegEnd::Stuck) {
            if (subj) { if (how == LegEnd::Blocked) { ++F.t.pathBlocked; if (n.hadBlock && std::hypot(x.c.x - n.blockX, x.c.y - n.blockY) < g_envT.reblockU) ++F.t.pathReblock; } }
            if (how == LegEnd::Blocked) { n.hadBlock = true; n.blockX = x.c.x; n.blockY = x.c.y; }
            n.holdUntil = F.now + static_cast<double>(g_envT.retrySec);
            // ENGINE DROPPED (apmf/OffenseApproach.cpp:409, 11-17f): a follower's approach on a foe the engine cannot path to ends with a timed backoff
            // (Tuning::offenseApproachBackoffSec), at most offenseApproachDropCap times per (follower, foe) per fight. Past the cap the foe is blocked.
            if (g_v4.caststart && how == LegEnd::Unreachable && x.ctl >= 0 && n.goalBody) { Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; const Body* gb = F.Find(n.goalBody);
                if (gb && gb->role == Role::Foe) { const Tuning& lt = *ActiveTuning(); int* cnt = nullptr; for (auto& kv : c.apprDrops) if (kv.first == n.goalBody) cnt = &kv.second;
                    if (!cnt) { c.apprDrops.push_back({ n.goalBody, 0 }); cnt = &c.apprDrops.back().second; }
                    if (++*cnt > static_cast<int>(lt.offenseApproachDropCap)) { c.apprBlocked.push_back(n.goalBody); if (subj) ++F.t.apprBlocked; }
                    else { n.holdUntil = F.now + static_cast<double>(std::max(0.0f, lt.offenseApproachBackoffSec)); if (subj) ++F.t.apprDrops; } } }
        } else if (subj) ++F.t.pathStuckLegs;
        if (F.trace && x.ctl >= 0) std::fprintf(F.trace, "%7.2f  LEG %s: %s at (%.0f, %.0f, %.0f)\n", F.now, x.c.name.c_str(), how == LegEnd::Blocked ? "BLOCKED" : how == LegEnd::Unreachable ? "UNREACHABLE" : "STUCK", x.c.x, x.c.y, x.c.z);
        n.n = n.i = 0; n.legOpen = false;
    }

    // One step toward (qx, qy) on the TRUE mesh: the straight walk, one slide along what stopped it, the door, the bodies.
    inline bool EnvTryStep(Fight& F, Body& x, float qx, float qy, float step) {
        Nav& n = x.nav; const EnvTuning& T = g_envT; const EQ::View v = EnvViewOf(F); const EQ::Walker who = WalkerOf(x);
        const float dx = qx - x.c.x, dy = qy - x.c.y, d = std::hypot(dx, dy); if (d < 1e-3f) return false;
        const float k = std::min(step, d) / d; const EQ::Vec3 from{ x.c.x, x.c.y, x.c.z }, to{ from.x + dx * k, from.y + dy * k, from.z };
        EQ::Walk w = EQ::WalkLine(v, n.tri, from, to, who);
        if (!w.reached && w.door >= 0) {
            const EQ::Door& dr = F.doors[static_cast<std::size_t>(w.door)];
            if (who.opensDoors && (dr.state == EQ::DoorState::Closed || who.unlocks)) {
                if (n.door != w.door) { n.door = w.door; n.doorAt = F.now + static_cast<double>(T.doorOpenSec); }
                else if (F.now >= n.doorAt) { F.OpenDoor(w.door); n.door = -1; if (x.ctl == 0) ++F.t.pathDoors; }
                n.progAt = F.now;   // opening a door is not being stuck
            } else { if (n.blockSince < 0 || n.blocker != 0) { n.blockSince = F.now; n.blocker = 0; } else if (F.now - n.blockSince >= static_cast<double>(T.blockSec)) { n.blockSince = -1; EnvLegEnd(F, x, LegEnd::Blocked); return false; } }
        } else if (!w.reached && w.hitEdge >= 0) {   // slide along the edge that stopped him
            const EQ::Tri& ht = v.tris[static_cast<std::size_t>(w.hitTri)]; const EQ::Vec3& a = v.verts[ht.v[w.hitEdge]]; const EQ::Vec3& b = v.verts[ht.v[(w.hitEdge + 1) % 3]];
            const float ex = b.x - a.x, ey = b.y - a.y, el = std::hypot(ex, ey);
            if (el > 1e-3f) { const float pr = ((to.x - w.end.x) * ex + (to.y - w.end.y) * ey) / el;
                if (std::fabs(pr) > 0.5f) { const EQ::Walk w2 = EQ::WalkLine(v, w.tri, w.end, { w.end.x + ex / el * pr, w.end.y + ey / el * pr, w.end.z }, who); w.end = w2.end; w.tri = w2.tri; } }
        }
        EQ::Vec3 np = w.end; std::int32_t nt = w.tri;
        if (g_v4.bodies && !x.airborne) {
            const float rx = EnvRadius(x);
            auto hits = [&](const EQ::Vec3& p) -> const Body* { for (const Body& o : F.b) { if (&o == &x || !Fight::Up(o) || o.airborne || o.perch || std::fabs(o.c.z - p.z) > 100.0f) continue;
                    if (o.Party() == x.Party() && F.now < n.passUntil) continue;
                    const float rr = rx + EnvRadius(o), dn = std::hypot(p.x - o.c.x, p.y - o.c.y); if (dn < rr && dn < std::hypot(from.x - o.c.x, from.y - o.c.y) - 1e-3f) return &o; } return nullptr; };
            if (const Body* o = hits(np)) {
                float tx = -(o->c.y - from.y), ty = o->c.x - from.x; const float tl = std::max(1e-3f, std::hypot(tx, ty)); tx /= tl; ty /= tl; if (tx * dx + ty * dy < 0.0f) { tx = -tx; ty = -ty; }
                const float sl = std::min(step, d); const EQ::Walk w3 = EQ::WalkLine(v, n.tri, from, { from.x + tx * sl, from.y + ty * sl, from.z }, who);
                if (std::hypot(w3.end.x - from.x, w3.end.y - from.y) > 0.5f * sl && !hits(w3.end)) { np = w3.end; nt = w3.tri; }
                else {
                    // A friend who is himself walking is a queue, not a block: he waits his turn (a doorway holds one body). A friend standing in
                    // his way lets him through after yieldSec (ASSUMPTION: the engine's soft avoidance). A foe never does.
                    const bool friendly = o->Party() == x.Party(), still = F.now - o->movedAt > 0.3;
                    if (friendly && !still) { n.blockSince = -1; n.progAt = F.now; return false; }
                    if (n.blockSince < 0 || n.blocker != o->c.id) { n.blockSince = F.now; n.blocker = o->c.id; }
                    if (friendly && F.now - n.blockSince >= static_cast<double>(T.yieldSec)) { n.passUntil = F.now + static_cast<double>(T.passSec); n.blockSince = -1; }
                    else if (F.now - n.blockSince >= static_cast<double>(T.blockSec)) { n.blockSince = -1; EnvLegEnd(F, x, LegEnd::Blocked); }
                    return false;
                }
            }
        }
        const float moved = std::hypot(np.x - from.x, np.y - from.y); if (moved < 1e-4f) return false;
        if (w.door < 0) n.blockSince = -1;
        x.c.x = np.x; x.c.y = np.y; x.c.z = np.z; n.tri = nt; x.movedAt = F.now; n.walked += moved; if (x.ctl == 0) F.t.pathWalked += moved;
        return true;
    }

    // A push that is not a walk of his own (a dodge step): straight, stops at the wall.
    inline void EnvShove(Fight& F, Body& x, float dx, float dy) {
        if (x.nav.tri < 0) EnvAdopt(F, x); if (x.nav.tri < 0) return;
        const EQ::Walk w = EQ::WalkLine(EnvViewOf(F), x.nav.tri, { x.c.x, x.c.y, x.c.z }, { x.c.x + dx, x.c.y + dy, x.c.z }, WalkerOf(x));
        x.c.x = w.end.x; x.c.y = w.end.y; x.c.z = w.end.z; x.nav.tri = w.tri; x.nav.n = x.nav.i = 0;
    }

    // Where a seek to (tx, ty) ends on the mesh: the level of whoever stands there (his target, the player), else his own.
    inline std::int32_t EnvGoal(const Fight& F, const Body& x, float tx, float ty, EQ::Vec3& gp, std::uint32_t& who) {
        const EQ::View v = EnvViewOf(F, true); float z = x.c.z, bd = 300.0f; who = 0;
        for (const Body& o : F.b) { if (&o == &x || !Fight::Up(o) || o.airborne) continue; const float d = std::hypot(o.c.x - tx, o.c.y - ty); if (d < bd) { bd = d; z = o.c.z; who = o.c.id; } }
        std::int32_t t = EQ::TriAt(v, { tx, ty, z });
        if (t >= 0) { gp = { tx, ty, EQ::HeightAt(v, t, tx, ty) }; if (std::fabs(gp.z - z) <= 200.0f) return t; }
        return EQ::NearestTri(v, { tx, ty, z }, g_envT.snapU, gp);
    }

    // A leg is over (he arrived, or a new one begins): its detour sample (2.5), walked over the ground it covered, for a leg that had a route.
    inline void EnvLegClose(Fight& F, Body& x) {
        Nav& n = x.nav;
        if (n.legOpen && n.legReach && x.ctl == 0 && n.walked > 200.0f) { const float straight = std::max(50.0f, std::hypot(x.c.x - n.legSx, x.c.y - n.legSy)); const double r = static_cast<double>(n.walked / straight);
            F.t.detourSum += r; ++F.t.detourN; ++F.t.detourHist[std::clamp(static_cast<int>((r - 1.0) * 4.0), 0, 15)];
            if (F.trace && r > 2.0) std::fprintf(F.trace, "%7.2f  DETOUR %s: walked %.0f for %.0f of ground, from (%.0f, %.0f) toward (%.0f, %.0f)\n", F.now, x.c.name.c_str(), static_cast<double>(n.walked), static_cast<double>(straight), n.legSx, n.legSy, n.legG0x, n.legG0y); }
        n.legOpen = false; n.walked = 0.0f;
    }

    inline void EnvPlan(Fight& F, Body& x, std::int32_t gt, const EQ::Vec3& gp, bool idleBefore) {
        Nav& n = x.nav; const EnvTuning& T = g_envT; const bool subj = x.ctl == 0;
        n.pathAt = F.now; n.stamp = F.doorStamp; n.goalTri = gt; n.goal = gp; n.i = n.n = 0; n.cut = n.blocked = false; n.reach = false;
        const EQ::Vec3 pos{ x.c.x, x.c.y, x.c.z };
        if (gt >= 0) {
            const EQ::View pv = EnvViewOf(F, true); const EQ::Walker who = WalkerOf(x);
            if (gt == n.tri || EQ::WalkLine(pv, n.tri, pos, gp, who).reached) { n.pts[0] = { gp, gt, EQ::Corner::Goal }; n.n = 1; n.reach = true; }
            else { const SimEnv::Route r = F.env->FindRoute(F.doors, F.doorStamp, n.tri, gt, who, true);
                if (r.any) { n.n = r.n; for (std::uint8_t i = 0; i < r.n; ++i) n.pts[i] = r.pts[i]; n.reach = r.reach; n.blocked = r.blocked; n.cut = r.cut;
                    if (n.reach && !n.cut && n.n && n.pts[n.n - 1].kind == EQ::Corner::Goal) n.pts[n.n - 1].p = gp; } }
        }
        if (subj) ++F.t.pathPlans;
        // legs (2.5): a new one when the goal moved far, or he had stopped asking
        const bool newLeg = !n.legOpen || idleBefore || std::hypot(gp.x - n.legG0x, gp.y - n.legG0y) > T.legGoalU;   // against the goal the leg BEGAN with: a chase is a row of short legs
        if (newLeg) {
            EnvLegClose(F, x);
            float fx = (n.n ? n.pts[0].p.x : gp.x) - x.c.x, fy = (n.n ? n.pts[0].p.y : gp.y) - x.c.y; const float fl = std::hypot(fx, fy);
            const bool longLeg = std::hypot(gp.x - x.c.x, gp.y - x.c.y) >= T.legMinU;
            if (fl > 1.0f) { fx /= fl; fy /= fl; if (subj && longLeg && n.legLong && F.now - n.legAt < static_cast<double>(T.oscSec) && fx * n.legDx + fy * n.legDy < -0.5f) { ++F.t.pathOsc;
                    if (F.trace) std::fprintf(F.trace, "%7.2f  REVERSAL %s: a new leg to (%.0f, %.0f) heads back %.1f s after the last began toward (%.0f, %.0f)\n", F.now, x.c.name.c_str(), gp.x, gp.y, F.now - n.legAt, n.legG0x, n.legG0y); }
                n.legDx = fx; n.legDy = fy; }
            n.legLong = longLeg;
            n.legAt = F.now; n.legSx = x.c.x; n.legSy = x.c.y; n.legG0x = gp.x; n.legG0y = gp.y; n.walked = 0.0f; n.legOpen = true;
            if (subj) { ++F.t.pathLegs; if (!n.reach) ++F.t.pathUnreach; }
        }
        n.legGx = gp.x; n.legGy = gp.y; n.legReach = n.reach;
        if (n.n == 0) EnvLegEnd(F, x, LegEnd::Unreachable);   // no mesh near the goal, or no route of any kind
    }

    inline void EnvMove(Fight& F, Body& x, float tx, float ty, float speed, bool away) {
        Nav& n = x.nav; const EnvTuning& T = g_envT;
        if (n.tri < 0) { EnvAdopt(F, x); if (n.tri < 0) return; }
        if (F.now < n.dropUntil) return;   // falling
        const float step = speed * TM::dt; const bool idleBefore = F.now - n.wantAt > 0.5;
        if (F.now - n.wantAt > 3.0 * static_cast<double>(TM::dt)) { n.progAt = F.now; n.px = x.c.x; n.py = x.c.y; n.stuckSince = -1; n.stuckNoted = false; }
        n.wantAt = F.now; bool arrived = false;
        if (away || !g_v4.path) { EnvTryStep(F, x, tx, ty, step); if (away) { n.n = n.i = 0; n.legOpen = false; } }
        else if (F.now >= n.holdUntil) {
            EQ::Vec3 gp; std::uint32_t who = 0; const std::int32_t gt = EnvGoal(F, x, tx, ty, gp, who); n.goalBody = who;
            // the offense approach's drop cap (apmf/OffenseApproach.cpp): past it the foe is not walked to again this fight
            if (g_v4.caststart && x.ctl >= 0 && who) { const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; if (std::find(c.apprBlocked.begin(), c.apprBlocked.end(), who) != c.apprBlocked.end()) return; }
            const bool spent = n.n == 0 || n.i >= n.n, stale = n.stamp != F.doorStamp;
            if (spent || stale || (gt != n.goalTri && F.now - n.pathAt >= static_cast<double>(T.repathSec))) EnvPlan(F, x, gt, gp, idleBefore);
            else if (gt == n.goalTri && n.reach && !n.cut && n.pts[n.n - 1].kind == EQ::Corner::Goal) n.pts[n.n - 1].p = gp;
            if (n.i < n.n) {
                const EQ::PathPoint c = n.pts[n.i];
                EnvTryStep(F, x, c.p.x, c.p.y, step);
                if (n.i < n.n && std::hypot(c.p.x - x.c.x, c.p.y - x.c.y) < 6.0f) {   // (a BLOCKED end inside the step emptied the route)
                    if (c.kind == EQ::Corner::DropStart && n.i + 1 < n.n && n.pts[n.i + 1].kind == EQ::Corner::DropLand) {
                        n.dropUntil = F.now + static_cast<double>(T.dropSec); n.dropTo = n.pts[n.i + 1].p; n.dropTri = n.pts[n.i + 1].tri; n.i = static_cast<std::uint8_t>(n.i + 2); if (x.ctl == 0) ++F.t.pathDrops; }
                    else ++n.i;
                    if (n.i >= n.n) { if (n.reach && !n.cut) arrived = true; else if (!n.cut) EnvLegEnd(F, x, n.blocked ? LegEnd::Blocked : LegEnd::Unreachable); }
                }
            }
        }
        // stuck (2.5): under stuckU of ground in stuckSec while a leg wants him to move
        if (!arrived && !away && g_v4.path && n.legOpen && n.reach && std::hypot(n.goal.x - x.c.x, n.goal.y - x.c.y) < 24.0f) arrived = true;   // at his goal (a mover that stops a step short never reaches the last corner)
        if (arrived) EnvLegClose(F, x);
        if (arrived || F.now < n.dropUntil) { n.progAt = F.now; n.px = x.c.x; n.py = x.c.y; }
        if (std::hypot(x.c.x - n.px, x.c.y - n.py) > T.stuckU) { n.px = x.c.x; n.py = x.c.y; n.progAt = F.now; n.stuckNoted = false; n.stuckSince = -1; }
        else if (F.now - n.progAt > static_cast<double>(T.stuckSec)) {
            if (x.ctl == 0) F.t.pathStuckSec += TM::dt;
            if (n.stuckSince < 0) n.stuckSince = F.now;
            if (!n.stuckNoted) { n.stuckNoted = true; if (F.now >= n.holdUntil) EnvLegEnd(F, x, LegEnd::Stuck); }
            if (x.ctl >= 0 && F.now - n.stuckSince > 30.0) { ++F.t.envStuckForever; n.stuckSince = F.now; }
        }
    }

    // Once a step: drops land, the hold-unreachable tally, the sanity counts (a NaN or an off-mesh body is a bug, counted and never hidden).
    inline void EnvStep(Fight& F) {
        const EQ::View v = EnvViewOf(F);
        for (Body& x : F.b) { if (!x.alive) continue; Nav& n = x.nav;
            if (n.dropTri >= 0 && F.now >= n.dropUntil) { x.c.x = n.dropTo.x; x.c.y = n.dropTo.y; x.c.z = n.dropTo.z; n.tri = n.dropTri; n.dropTri = -1; x.movedAt = F.now; }
            if (!std::isfinite(x.c.x) || !std::isfinite(x.c.y) || !std::isfinite(x.c.z)) { ++F.t.envBad; continue; }
            if (x.airborne || x.perch) continue;
            if (n.tri < 0) { ++F.t.envBad; continue; } }
        if (F.sanityAt <= F.now) { F.sanityAt = F.now + 1.0;   // once a second: is every walker on the triangle he believes he is on (within 8 units in x, y)?
            for (const Body& x : F.b) { if (!Fight::Up(x) || x.airborne || x.perch || x.nav.tri < 0 || F.now < x.nav.dropUntil) continue;
                const float out = EQ::OutsideBy(v, x.nav.tri, x.c.x, x.c.y);
                if (out > 8.0f) { ++F.t.envBad; if (std::getenv("SIM_ENVBAD")) std::fprintf(stderr, "ENVBAD %s %s at (%.1f %.1f %.1f) is %.1f outside his triangle %d, t %.1f\n", F.env->name.c_str(), x.c.name.c_str(), x.c.x, x.c.y, x.c.z, static_cast<double>(out), x.nav.tri, F.now); } } }
        if (!F.ctl.empty()) { const Ctl& c0 = F.ctl[0]; const Body& s0 = F.b[static_cast<std::size_t>(c0.body)];
            if (Fight::Up(s0)) { const Body* tg = F.Find(c0.steer ? c0.steer : s0.c.targetId);
                // holdUnreach (2.5): his attack is on a foe he cannot reach and cannot shoot (no ranged way in his hands, or no sight). Reach = the last route he asked for toward that foe.
                const bool canShoot = c0.drawn == PostureKind::Bow || c0.drawn == PostureKind::Staff || (c0.gate.spellsOnClass && c0.drawn == PostureKind::None);
                const bool capped = tg && std::find(c0.apprBlocked.begin(), c0.apprBlocked.end(), tg->c.id) != c0.apprBlocked.end();
                if (tg && Fight::Up(*tg) && !tg->Party() && !tg->airborne && (capped || (s0.nav.goalTri >= 0 && !s0.nav.reach && F.now - s0.nav.wantAt < 1.5 + static_cast<double>(ActiveTuning()->offenseApproachBackoffSec) && s0.nav.goalBody == tg->c.id)) && (!canShoot || !EnvSees(F, s0, *tg))) F.t.pathHoldUnreachSec += TM::dt; } }
    }

}
