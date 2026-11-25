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
    [[nodiscard]] inline EQ::View EnvViewOf(const Fight& F, bool plan = false) {
        EQ::View v = F.env->View(F.doors, F.doorStamp, plan);
        if (F.meshEdit && !plan) { v.tris = F.meshEdit->tris; v.walls = F.meshEdit->walls; v.grid = &F.meshEdit->grid; v.stamp ^= F.meshEdit->stamp; }
        return v;
    }
    // The truth becomes what the planner's mesh says (a fixture's DFCT defect is repaired, an edit is taken back).
    inline void EnvRepair(Fight& F) {
        if (F.env->planTris.empty()) { F.meshEdit = nullptr; return; }
        auto e = std::make_shared<MeshEdit>(); e->tris = F.env->planTris; e->walls = F.env->walls; e->grid = F.env->grid; e->stamp = SimEnv::Mix(0x4E9A14ULL); F.meshEdit = std::move(e);
    }
    // Edit the truth of this fight: a_cut says which edges are gone (both sides are asked), a_faces = cliff-like faces the rays meet (a wall the mesh does not know).
    template <class Cut> inline void EnvEdit(Fight& F, Cut&& a_cut, const std::vector<EQ::Wall>& a_faces = {}) {
        auto e = std::make_shared<MeshEdit>(); const EQ::View v = EnvViewOf(F); e->tris.assign(v.tris.begin(), v.tris.end()); e->walls.assign(v.walls.begin(), v.walls.end()); e->grid = *v.grid;
        for (std::size_t i = 0; i < e->tris.size(); ++i) for (int k = 0; k < 3; ++k) if (e->tris[i].nb[k] >= 0 && a_cut(v, static_cast<std::int32_t>(i), e->tris[i].nb[k])) { e->tris[i].nb[k] = -1; e->tris[i].kind[k] = EQ::Edge::Wall; }
        e->walls.insert(e->walls.end(), a_faces.begin(), a_faces.end()); EQ::IndexWalls(e->grid, e->walls);
        e->stamp = SimEnv::Mix((F.meshEdit ? F.meshEdit->stamp : 0) + 1); F.meshEdit = std::move(e);
    }
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
            if (EQ::CastRay(v, eye, to, tl_envScratch, g_sightT, T.targetMargin).clear) return true;
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
        if (std::fabs(EQ::HeightAt(v, t, px, py) - who.c.z) > g_envT.storeyU) return false;
        const std::int32_t d = v.tris[static_cast<std::size_t>(t)].door; return d < 0 || F.doors[static_cast<std::size_t>(d)].leaf || F.doors[static_cast<std::size_t>(d)].state == EQ::DoorState::Open;   // a door with a leaf closes the leaf, not its triangle
    }

    // Can a blow pass between them: the straight walk from one to the other is open floor (no wall, no closed door, no ledge), on one level.
    inline bool EnvMeleeClear(const Fight& F, const Body& a, const Body& o) {
        if (a.nav.tri < 0 || std::fabs(a.c.z - o.c.z) > 110.0f) return a.nav.tri < 0;
        const EQ::Walk w = EQ::WalkLine(EnvViewOf(F), a.nav.tri, { a.c.x, a.c.y, a.c.z }, { o.c.x, o.c.y, o.c.z }, EQ::Walker{});
        return (w.reached || std::hypot(w.end.x - o.c.x, w.end.y - o.c.y) < 24.0f) && std::fabs(w.end.z - o.c.z) < 110.0f;   // ... and it ends on HIS floor, not the one under it
    }

    // Does a blast or an area centred at (cx, cy, cz) reach him: inside its radius in 3D, with a clear ray from the centre (review F2: a fireball in the
    // next room, or on the floor below, does not burn through the wall). The ray runs chest to chest.
    inline bool EnvInBlast(const Fight& F, float cx, float cy, float cz, float r, const Body& o) {
        if (std::hypot(o.c.x - cx, o.c.y - cy, o.c.z - cz) > r) return false;
        if (!g_v4.los) return true;
        const float h = g_envT.torsoFrac * g_envT.bodyH; const EQ::Vec3 a{ cx, cy, cz + h }, b{ o.c.x, o.c.y, o.c.z + h };
        if (std::hypot(b.x - a.x, b.y - a.y, b.z - a.z) <= g_envT.targetMargin) return true;
        return EQ::CastRay(EnvViewOf(F), a, b, tl_envScratch, g_sightT, g_envT.targetMargin).clear;
    }

    // Does he hold the offense approach claim when he walks at a foe: a follower who walks in for SIGHT (a caster, a bow in his hands). A blade
    // pursues by the engine's own combat movement, which the claim's backoff, cap and block never bound.
    inline bool EnvClaimWalker(const Fight& F, const Body& x) {
        return x.ctl >= 0 && (x.casterMode || x.rangedMode || F.ctl[static_cast<std::size_t>(x.ctl)].drawn == PostureKind::Bow);
    }
    // Is his approach on this foe blocked NOW. A block lapses (the foe may be walked to again) when he stands more than blockLapseU from where it
    // was made (native BlockStands, kBlockMoveUnits: "things moved on") or a door has changed since (the lab's reason the route was undone).
    inline bool EnvApprBlocked(Fight& F, const Body& x, std::uint32_t foe) {
        if (!EnvClaimWalker(F, x)) return false;
        Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)];
        for (std::size_t i = 0; i < c.apprBlocked.size(); ++i) { const Ctl::ApprBlock& q = c.apprBlocked[i]; if (q.foe != foe) continue;
            if (std::hypot(x.c.x - q.x, x.c.y - q.y) <= g_envT.blockLapseU && q.stamp == F.doorStamp) return true;
            if (F.trace) std::fprintf(F.trace, "%7.2f  BLOCK LAPSED %s on %u (%s)\n", F.now, x.c.name.c_str(), foe, q.stamp == F.doorStamp ? "he moved on" : "a door changed");
            c.apprBlocked.erase(c.apprBlocked.begin() + static_cast<std::ptrdiff_t>(i)); return false; }
        return false;
    }
    enum class LegEnd { Blocked, Unreachable, Stuck };
    inline void MoveLegEnd(Fight& F, Body& x, LegEnd how, const EQ::Vec3& toward);   // S2 (MoveWorld.h): a leg toward a spot claim ended
    inline void EnvLegEnd(Fight& F, Body& x, LegEnd how) {
        Nav& n = x.nav; const bool subj = x.ctl == 0;
        // AN END BESIDE HIS LAST END OF THE SAME KIND IS A REPEAT (review F8: only a BLOCKED end was compared, so asking again and again for a route
        // that does not exist, or sticking again and again at one corner, cost nothing). BLOCKED repeats stay Tally::pathReblock; the other two are pathRepeat.
        { const int k = static_cast<int>(how); const bool rep = n.hadEnd[k] && std::hypot(x.c.x - n.endX[k], x.c.y - n.endY[k]) < g_envT.reblockU;
          if (subj && rep) { if (how == LegEnd::Blocked) ++F.t.pathReblock; else ++F.t.pathRepeat; }
          n.hadEnd[k] = true; n.endX[k] = x.c.x; n.endY[k] = x.c.y; }
        if (how != LegEnd::Stuck) {
            if (subj && how == LegEnd::Blocked) ++F.t.pathBlocked;
            n.holdUntil = F.now + static_cast<double>(g_envT.retrySec);
            // THE OFFENSE APPROACH'S OWN BOUNDS (apmf/OffenseApproach.cpp). They bound the ch.24 approach CLAIM, which only a follower walking in for
            // sight holds (a caster, a bow: EnvClaimWalker), never a blade's pursuit (re-check R2). A path the engine could not make is ENGINE
            // DROPPED: a timed backoff (Tuning::offenseApproachBackoffSec), at most offenseApproachDropCap times per (follower, foe) per fight,
            // then a block. A route that ends at a door he cannot open is the same undone path (native has no ending called "a door"), so it takes
            // the same road and is no longer a block for the fight at the first try. The block is scoped to where and when it was made (EnvApprBlocked).
            if (g_v4.caststart && (how == LegEnd::Unreachable || (how == LegEnd::Blocked && n.blocked)) && n.goalBody && EnvClaimWalker(F, x)) { Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; const Body* gb = F.Find(n.goalBody);
                if (gb && gb->role == Role::Foe) { const Tuning& lt = *ActiveTuning(); int* cnt = nullptr; for (auto& kv : c.apprDrops) if (kv.first == n.goalBody) cnt = &kv.second;
                    if (!cnt) { c.apprDrops.push_back({ n.goalBody, 0 }); cnt = &c.apprDrops.back().second; }
                    if (++*cnt > static_cast<int>(lt.offenseApproachDropCap)) { if (std::none_of(c.apprBlocked.begin(), c.apprBlocked.end(), [&](const Ctl::ApprBlock& q) { return q.foe == n.goalBody; })) { c.apprBlocked.push_back({ n.goalBody, x.c.x, x.c.y, F.doorStamp }); if (subj) ++F.t.apprBlocked; } }
                    else { n.holdUntil = F.now + static_cast<double>(std::max(0.0f, lt.offenseApproachBackoffSec)); if (subj) ++F.t.apprDrops; } } }
        } else { n.exact = true; n.exactUntil = F.now + 3.0; if (subj) ++F.t.pathStuckLegs; }
        if (F.trace && x.ctl >= 0) std::fprintf(F.trace, "%7.2f  LEG %s: %s at (%.0f, %.0f, %.0f) toward (%.0f, %.0f, %.0f), corner %d of %d%s%s, hold until %.1f\n", F.now, x.c.name.c_str(), how == LegEnd::Blocked ? "BLOCKED" : how == LegEnd::Unreachable ? "UNREACHABLE" : "STUCK", x.c.x, x.c.y, x.c.z, n.goal.x, n.goal.y, n.goal.z, int(n.i), int(n.n), n.reach ? "" : " (no route to the goal)", n.away ? " (away)" : "", n.holdUntil);
        const EQ::Vec3 toward = n.i < n.n ? n.pts[n.i].p : n.goal;   // before the route is emptied
        n.n = n.i = 0; n.legOpen = false; n.away = false;
        if (F.move && x.ctl >= 0) MoveLegEnd(F, x, how, toward);
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
                if (n.door < 0 || F.doors[static_cast<std::size_t>(n.door)].id != dr.id) { n.door = w.door; n.doorAt = F.now + static_cast<double>(T.doorOpenSec); }   // by the DOOR, not the row: a door on two triangles is one door (he slid from one to the other and the timer began again, for good)
                else if (F.now >= n.doorAt) { F.OpenDoor(w.door); n.door = -1; if (x.ctl == 0) ++F.t.pathDoors; if (F.trace) std::fprintf(F.trace, "%7.2f  DOOR %s opens door %u\n", F.now, x.c.name.c_str(), dr.id); }
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

    // Where a seek ends on the mesh. THE CALLER SAYS WHO OR WHAT HE WALKS TO (review F6): a body (its own triangle and height, so a target on
    // another storey is on that storey), an explicit mesh point, or neither, which means a point of HIS OWN storey. Nothing is guessed from who
    // happens to stand near the x, y.
    inline std::int32_t EnvGoal(const Fight& F, const Body& x, float tx, float ty, const Body* to, const EnvPoint* point, EQ::Vec3& gp) {
        if (point) { gp = point->p; return point->tri; }
        const EQ::View v = EnvViewOf(F, true); const float z = to && to->nav.tri >= 0 ? to->c.z : x.c.z;
        if (to && to->nav.tri >= 0 && std::fabs(to->c.x - tx) < 0.5f && std::fabs(to->c.y - ty) < 0.5f) { gp = { to->c.x, to->c.y, to->c.z }; return to->nav.tri; }
        const std::int32_t t = EQ::TriAt(v, { tx, ty, z });
        if (t >= 0) { gp = { tx, ty, EQ::HeightAt(v, t, tx, ty) }; if (std::fabs(gp.z - z) <= g_envT.storeyU) return t; }
        return EQ::NearestTri(v, { tx, ty, z }, g_envT.snapU, gp);
    }

    // A leg is over. Its detour sample (2.5) is taken when he ARRIVED: what he walked over the length of the route he was given (FindPath's, or the
    // straight line of a direct walk), so a route round a wall is not a detour and a walk that wandered off its route is. The route is measured
    // FROM WHERE HE STOOD (EnvPlan), and how far the goal ended from where it began (the NET displacement of a foe he chases, at most
    // legGoalU by the leg's own rule) is added to it, so following a moving foe reads about 1 and not 3 (review F8: a route of 100 and a foe who
    // walked 150 read 2.5). Net, not the goal's whole travel (re-check): a foe pacing back and forth would otherwise excuse any walk.
    inline void EnvLegClose(Fight& F, Body& x, bool arrived) {
        Nav& n = x.nav;
        if (arrived) n.exact = false;
        if (arrived && n.legOpen && n.legReach && x.ctl == 0 && n.walked > 200.0f && n.legLen > 1.0f) { const double r = static_cast<double>(n.walked / std::max(50.0f, n.legLen + std::hypot(n.goal.x - n.legG0x, n.goal.y - n.legG0y)));
            F.t.detourSum += r; ++F.t.detourN; ++F.t.detourHist[std::clamp(static_cast<int>((r - 1.0) * 4.0), 0, 15)];
            if (F.trace && r > 2.0) std::fprintf(F.trace, "%7.2f  DETOUR %s: walked %.0f on a route of %.0f, from (%.0f, %.0f) toward (%.0f, %.0f)\n", F.now, x.c.name.c_str(), static_cast<double>(n.walked), static_cast<double>(n.legLen), n.legSx, n.legSy, n.legG0x, n.legG0y); }
        n.legOpen = false; n.walked = 0.0f;
    }
    // A leg begins, heading (fx, fy). A reversal (2.5) is a leg that heads back within oscSec of the one before, when both go farther than legMinU.
    // AWAY legs (a backpedal, a flight) are legs too (review F7): walking at a foe and backing off him again and again is the oscillation.
    inline void EnvLegStart(Fight& F, Body& x, float fx, float fy, float goalDist, float gx, float gy, float len, bool reach) {
        Nav& n = x.nav; const EnvTuning& T = g_envT; const bool subj = x.ctl == 0, longLeg = goalDist >= T.legMinU; const float fl = std::hypot(fx, fy);
        if (fl > 1e-3f) { fx /= fl; fy /= fl;
            if (subj && longLeg && n.legLong && F.now - n.legAt < static_cast<double>(T.oscSec) && fx * n.legDx + fy * n.legDy < -0.5f) { ++F.t.pathOsc;
                if (F.trace) std::fprintf(F.trace, "%7.2f  REVERSAL %s: a new leg to (%.0f, %.0f) heads back %.1f s after the last began toward (%.0f, %.0f)\n", F.now, x.c.name.c_str(), gx, gy, F.now - n.legAt, n.legG0x, n.legG0y); }
            n.legDx = fx; n.legDy = fy; }
        n.legLong = longLeg; n.legAt = F.now; n.legSx = x.c.x; n.legSy = x.c.y; n.legG0x = gx; n.legG0y = gy; n.walked = 0.0f; n.legOpen = true; n.legLen = len; n.legReach = reach;
        if (subj) { ++F.t.pathLegs; if (!reach) ++F.t.pathUnreach; }
    }

    inline void EnvPlan(Fight& F, Body& x, std::int32_t gt, const EQ::Vec3& gp, bool idleBefore) {
        Nav& n = x.nav; const EnvTuning& T = g_envT; const bool subj = x.ctl == 0;
        n.pathAt = F.now; n.stamp = F.doorStamp; n.goalTri = gt; n.goal = gp; n.i = n.n = 0; n.cut = n.blocked = false; n.reach = false; n.away = false;
        const EQ::Vec3 pos{ x.c.x, x.c.y, x.c.z }; float len = 0.0f;
        if (gt >= 0) {
            const bool truth = F.engineTrue; const EQ::View pv = EnvViewOf(F, !truth); const EQ::Walker who = WalkerOf(x);
            // the straight walk counts only when it ENDS AT THE GOAL'S HEIGHT (review F1): the same x, y on the floor above is not the goal
            if (EQ::WalkLine(pv, n.tri, pos, gp, who).At()) { n.pts[0] = { gp, gt, EQ::Corner::Goal }; n.n = 1; n.reach = true; len = std::hypot(gp.x - pos.x, gp.y - pos.y, gp.z - pos.z); }
            else if ((n.exact && F.now < n.exactUntil) || truth) {   // the shared route runs centre to centre, and its first corner need not be a straight walk from where HE is. After a STUCK end the route is searched from his own spot (a pure function of the state, so still the same run at any thread count)
                static thread_local EQ::Path xp; EQ::PathQuery q; q.from = pos; q.to = gp; q.fromTri = n.tri; q.toTri = gt; q.who = who;
                if (EQ::FindPath(pv, q, tl_envScratch, xp)) { n.n = static_cast<std::uint8_t>(std::min<std::size_t>(xp.pts.size(), n.pts.size())); for (std::uint8_t i = 0; i < n.n; ++i) n.pts[i] = xp.pts[i];
                    n.reach = xp.reachable; n.blocked = xp.blocked; n.cut = xp.pts.size() > n.pts.size(); len = xp.length; } }
            else { const SimEnv::Route r = F.env->FindRoute(F.doors, F.doorStamp, n.tri, gt, who, true);
                if (r.any) { n.n = r.n; for (std::uint8_t i = 0; i < r.n; ++i) n.pts[i] = r.pts[i]; n.reach = r.reach; n.blocked = r.blocked; n.cut = r.cut; len = r.length;
                    if (n.reach && !n.cut && n.n && n.pts[n.n - 1].kind == EQ::Corner::Goal) n.pts[n.n - 1].p = gp; } }
        }
        if (n.n && n.reach && !n.cut) { len = 0.0f; EQ::Vec3 pv2 = pos; for (std::uint8_t i = 0; i < n.n; ++i) { len += std::hypot(n.pts[i].p.x - pv2.x, n.pts[i].p.y - pv2.y, n.pts[i].p.z - pv2.z); pv2 = n.pts[i].p; } }   // the route he was GIVEN, from where he stands (the cached length runs centre to centre)
        else len = std::max(len, std::hypot(gp.x - pos.x, gp.y - pos.y, gp.z - pos.z));
        if (subj) ++F.t.pathPlans;
        if (!n.legOpen || idleBefore || std::hypot(gp.x - n.legG0x, gp.y - n.legG0y) > T.legGoalU) {   // against the goal the leg BEGAN with: a chase is a row of short legs
            EnvLegClose(F, x, false);
            EnvLegStart(F, x, (n.n ? n.pts[0].p.x : gp.x) - x.c.x, (n.n ? n.pts[0].p.y : gp.y) - x.c.y, std::hypot(gp.x - x.c.x, gp.y - x.c.y), gp.x, gp.y, len, n.reach); }
        n.legReach = n.reach;
        if (n.n == 0) EnvLegEnd(F, x, LegEnd::Unreachable);   // no mesh near the goal, or no route of any kind
    }

    // THE FALLBACK POINT of a backpedal or a flight (review F3). The engine's fallback and back-off are PATHED moves (Docs/research dodge-research.md:155),
    // not a push along a bearing, so he does not walk into a wall and keep walking. From him, nine straight walks fanned over the half circle that
    // leads away from what he leaves, each up to awayU long. The one that ends farthest from it wins, when it gains at least awayGainU. When none
    // does he is CORNERED and stands (counted apart: Tally::pathCorneredSec, never as stuck).
    inline bool EnvAwayPick(const Fight& F, const Body& x, float fromX, float fromY, const Body* keep, EQ::Vec3& out, std::int32_t& outTri) {
        const EnvTuning& T = g_envT; const EQ::View v = EnvViewOf(F); const EQ::Walker who = WalkerOf(x); const EQ::Vec3 pos{ x.c.x, x.c.y, x.c.z };
        const float base = std::atan2(x.c.y - fromY, x.c.x - fromX), now0 = std::hypot(x.c.x - fromX, x.c.y - fromY); float best = now0 + T.awayGainU; bool have = false, haveSeen = false;
        for (int i = 0; i < 9; ++i) { const float a = base + static_cast<float>(((i + 1) / 2) * ((i & 1) ? 1 : -1)) * 0.3927f;   // 0, +-22.5, +-45, +-67.5, +-90 degrees: straight back first
            const EQ::Walk w = EQ::WalkLine(v, x.nav.tri, pos, { pos.x + std::cos(a) * T.awayU, pos.y + std::sin(a) * T.awayU, pos.z }, who);
            const float d = std::hypot(w.end.x - fromX, w.end.y - fromY); if (d <= now0 + T.awayGainU) continue;
            // A back-off from a foe he fights (keep) PREFERS a point that still sees him (re-check R1): a step back behind the corner he just came
            // round is the dither. Any point with sight beats every point without. A flight (no keep) takes the farthest, as before.
            const bool seen = keep && g_v4.los && EnvSeesPoint(F, { w.end.x, w.end.y, w.end.z + T.eyeFrac * T.bodyH }, *keep);
            if ((seen && !haveSeen) || (seen == haveSeen && d > best + 1.0f)) { best = d; out = w.end; outTri = w.tri; have = true; haveSeen = seen; } }
        return have;
    }

    inline void EnvMove(Fight& F, Body& x, float tx, float ty, float speed, bool away, const Body* to, const EnvPoint* point) {
        Nav& n = x.nav; const EnvTuning& T = g_envT; const bool subj = x.ctl == 0;
        if (n.tri < 0) { EnvAdopt(F, x); if (n.tri < 0) return; }
        if (F.now < n.dropUntil) return;   // falling
        // The offense approach's block (apmf/OffenseApproach.cpp): a foe past the drop cap, or behind a door he cannot open, is not walked to again this
        // fight. No leg is filed, so this is not stuck time. It is HOLD time (Tally::pathHoldSec, the same tally the backoff before the cap feeds: review
        // F8), and HOLD-UNREACHABLE time too when his attack stays on that foe (EnvStep counts it).
        if (g_v4.caststart && g_v4.path && !away && to && EnvApprBlocked(F, x, to->c.id)) { if (subj) F.t.pathHoldSec += TM::dt; return; }
        const float step = speed * TM::dt; const bool idleBefore = F.now - n.wantAt > 0.5;
        // THE STUCK WINDOW COUNTS WANTED SECONDS (review F8). A gap in the asking (a swing, a stagger, a turn spent on a cast) is taken out of the window
        // and does not clear it, so a body that asks, fails, swings and asks again is stuck. Only a gap past wantGapSec is standing by choice.
        if (const double gap = F.now - n.wantAt - static_cast<double>(TM::dt); gap > static_cast<double>(T.wantGapSec)) { n.progAt = F.now; n.px = x.c.x; n.py = x.c.y; n.stuckSince = -1; n.stuckNoted = false; n.stuckWant = 0.0f; }
        else if (gap > 0.5 * static_cast<double>(TM::dt)) { n.progAt += gap; if (n.stuckSince >= 0) n.stuckSince += gap; }
        n.wantAt = F.now; bool arrived = false, cornered = false, held = false;
        if (!g_v4.path) EnvTryStep(F, x, tx, ty, step);
        else if (away) {
            const float fromX = 2.0f * x.c.x - tx, fromY = 2.0f * x.c.y - ty;   // every away caller asks for (him + (him - what he leaves))
            if (!n.away || F.now >= n.awayUntil || std::hypot(n.goal.x - x.c.x, n.goal.y - x.c.y) < 12.0f) {
                EQ::Vec3 p; std::int32_t pt = -1; const bool was = n.away && n.n > 0; n.awayUntil = F.now + static_cast<double>(to ? T.awayCommitSec : T.awayPickSec);   // a back-off commits to its pathed point, a flight follows the threat
                if (EnvAwayPick(F, x, fromX, fromY, to, p, pt)) { if (!was) { EnvLegClose(F, x, false); EnvLegStart(F, x, p.x - x.c.x, p.y - x.c.y, std::hypot(p.x - x.c.x, p.y - x.c.y), p.x, p.y, 0.0f, true); }
                    n.goal = p; n.goalTri = pt; n.pts[0] = { p, pt, EQ::Corner::Goal }; n.n = 1; n.i = 0; n.reach = true; }
                else { n.n = n.i = 0; if (n.legOpen) EnvLegClose(F, x, false); }
                n.away = true; n.goalBody = 0; }
            if (n.n) { if (!EnvTryStep(F, x, n.goal.x, n.goal.y, step)) n.awayUntil = F.now; } else cornered = true;   // a step that went nowhere ends the commitment: he picks again
            if (subj) { F.t.pathAwaySec += TM::dt; if (cornered) F.t.pathCorneredSec += TM::dt; }
        }
        else {
            EQ::Vec3 gp; const std::int32_t gt = EnvGoal(F, x, tx, ty, to, point, gp); n.goalBody = to ? to->c.id : 0;
            if (F.now >= n.holdUntil) {
                const bool spent = n.n == 0 || n.i >= n.n || n.away, stale = n.stamp != F.doorStamp;
                if (spent || stale || (gt != n.goalTri && F.now - n.pathAt >= static_cast<double>(T.repathSec))) EnvPlan(F, x, gt, gp, idleBefore);
                else if (gt == n.goalTri && n.reach && !n.cut && n.pts[n.n - 1].kind == EQ::Corner::Goal) { n.pts[n.n - 1].p = gp; n.goal = gp; }
                if (n.i < n.n) {
                    const EQ::PathPoint c = n.pts[n.i];
                    EnvTryStep(F, x, c.p.x, c.p.y, step);
                    if (n.i < n.n && std::hypot(c.p.x - x.c.x, c.p.y - x.c.y) < 6.0f && std::fabs(c.p.z - x.c.z) < T.levelU) {   // AT the corner: its height too (a BLOCKED end inside the step emptied the route)
                        if (c.kind == EQ::Corner::DropStart && n.i + 1 < n.n && n.pts[n.i + 1].kind == EQ::Corner::DropLand) {
                            n.dropUntil = F.now + static_cast<double>(T.dropSec); n.dropTo = n.pts[n.i + 1].p; n.dropTri = n.pts[n.i + 1].tri; n.i = static_cast<std::uint8_t>(n.i + 2); if (subj) ++F.t.pathDrops; }
                        else ++n.i;
                        if (n.i >= n.n) { if (n.reach && !n.cut) arrived = true; else if (!n.cut) EnvLegEnd(F, x, n.blocked ? LegEnd::Blocked : LegEnd::Unreachable); }
                    }
                    else if (n.i < n.n && std::hypot(c.p.x - x.c.x, c.p.y - x.c.y) < 6.0f) { n.n = n.i = 0; n.pathAt = -1e9; }   // over or under the corner, not at it: the route is void, ask again from where he really is
                }
                if (!arrived && n.legOpen && n.reach && std::hypot(n.goal.x - x.c.x, n.goal.y - x.c.y) < 24.0f && std::fabs(n.goal.z - x.c.z) < T.levelU) arrived = true;   // at his goal (a mover that stops a step short never reaches the last corner)
            }
            else held = true;
        }
        // WAITING OUT A HOLD (retrySec after a BLOCKED or an UNREACHABLE end, the offense approach's backoff) is not a leg that makes no progress: he has
        // no route and is not walking. It is counted as what it is (Tally::pathHoldSec, gated beside stuck%), and the stuck window stands still meanwhile.
        // Before review F8 the backoff fed stuck% while the same wait past the drop cap fed nothing.
        if (held) { if (subj) F.t.pathHoldSec += TM::dt; n.progAt += static_cast<double>(TM::dt); if (n.stuckSince >= 0) n.stuckSince += static_cast<double>(TM::dt); return; }
        if (arrived) EnvLegClose(F, x, true);
        // stuck (2.5): under stuckU of ground in stuckSec while a leg wants him to move. Standing cornered is his choice and is counted apart.
        if (arrived || cornered || F.now < n.dropUntil) { n.progAt = F.now; n.px = x.c.x; n.py = x.c.y; }
        if (std::hypot(x.c.x - n.px, x.c.y - n.py) > T.stuckU) { n.px = x.c.x; n.py = x.c.y; n.progAt = F.now; n.stuckNoted = false; n.stuckSince = -1; n.stuckWant = 0.0f; }
        else if (F.now - n.progAt > static_cast<double>(T.stuckSec)) {
            if (subj) F.t.pathStuckSec += TM::dt;
            if (n.stuckSince < 0) n.stuckSince = F.now;
            // The leg ends STUCK when the window fills, and AGAIN every stuckSec he stays stuck: each end asks for a route from where he stands
            // (Nav::exact) and each one beside the last is a repeat. One end per stuck spell left him on the shared route for good once exactUntil ran out.
            n.stuckWant += TM::dt;
            if (!n.stuckNoted || n.stuckWant >= T.stuckSec) { n.stuckNoted = true; n.stuckWant = 0.0f; EnvLegEnd(F, x, LegEnd::Stuck); }
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
                const bool capped = tg && EnvClaimWalker(F, s0) && std::any_of(c0.apprBlocked.begin(), c0.apprBlocked.end(), [&](const Ctl::ApprBlock& q) { return q.foe == tg->c.id; });
                if (tg && Fight::Up(*tg) && !tg->Party() && !tg->airborne && (capped || (s0.nav.goalTri >= 0 && !s0.nav.reach && F.now - s0.nav.wantAt < 1.5 + static_cast<double>(ActiveTuning()->offenseApproachBackoffSec) && s0.nav.goalBody == tg->c.id)) && (!canShoot || !EnvSees(F, s0, *tg))) F.t.pathHoldUnreachSec += TM::dt; } }
    }

}
