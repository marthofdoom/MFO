#pragma once
// iaus_sim/StrictWorld.h -- THE STRICT EXECUTOR (tight-movement-plan.md 11, hm1-seat-brief.md 7 and 15): what Harbinger's strict orders do to a body. HOST-ONLY.
// Included by MoveWorld.h. Nothing here decides: the order is native/iaus MoveOrder (MovePass::Orders), this carries it out (--position-road strict).
//
//   the pick     every Residue::selectorLag seconds (the engine's Movement Selector) he is asked: travel or stand. A crossing of the leash is seen then, not before
//   Travelling   along the sim's own mover to the point, until inside the arrival error. Backing away from the foe he faces he has Residue::kiteSpeed, not his run
//                across it strafeSpeed (Harbinger cannot release the aim before H-M3: RoadCaps::disengage gives GoTo and Disengage his full run). He FIGHTS on the
//                way, except in Disengage. A walk back has walkBackSpeed whatever its direction
//   Standing     no movement of his own at all. What moves him: a lunge, a dodge (cut at the leash), a shove, a stagger. Past leashR the next pick walks him back
//   Blocked      a leg that ended Blocked or Unreachable: he stands Residue::blockBackoff, then travels again. Three in blockWindow end the order (Unreachable)
//   the engine's Flee and Hide are never picked (marth: our order beats Flee / Hide). MFO's own runs (hide run, back-off, retreat, regroup) keep their precedence
//   until G2's one owner: the order waits under them, as on the point road.
// A fear EFFECT takes his order away for Residue::fearUncarried: he is the engine's, with Flee and Hide still denied.
// Free is not here: no order, the engine's own movement model, unchanged. THE CLIENT'S CONTRACT (s5-plan.md 4.1): Free ENDS the claim, so the lab bounds it by no
// radius and the order after it pays refileStall. M-1's Free path must do exactly that.
// NOT CARRIED YET (Harbinger CarryOf kNot, the pick is the engine's own): refileStall, startDelay, notCarriedShare. He moves as the engine moves him, Flee and Hide
// denied as under a fear. repointStall is the other thing: carried, one pick of standing.
#include "iaus_sim/World.h"

namespace Sim {

    [[nodiscard]] inline MoveClaim* StrictClaim(Fight& F, const Body& x) { return g_road == Road::Strict && F.move && x.ctl >= 0 && static_cast<std::size_t>(x.ctl) < F.move->claim.size() ? &F.move->claim[static_cast<std::size_t>(x.ctl)] : nullptr; }
    [[nodiscard]] inline const MoveClaim* StrictClaim(const Fight& F, const Body& x) { return StrictClaim(const_cast<Fight&>(F), x); }
    [[nodiscard]] inline float StrictHash(const Fight& F, const Body& x, std::uint64_t salt) { return static_cast<float>(Fight::Mix64(F.avSeed ^ Fight::Mix64(salt ^ (static_cast<std::uint64_t>(x.c.id) << 32))) >> 40) / 16777216.0f; }

    // ch.23: a leash wins over a point. The order is not carried while its point lies past it.
    [[nodiscard]] inline bool StrictLeashed(const Fight& F, const Body& x, const MoveClaim& q) { if (g_moveFault->noLeash) return false; const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; return c.leash > 0.0f && std::hypot(q.order.p.x - F.b[0].c.x, q.order.p.y - F.b[0].c.y) > c.leash + kLeashStandU; }
    // An order that is filed on this body now, and one that is carried too (a fear effect suspends it, Residue::fearUncarried).
    [[nodiscard]] inline bool StrictFiled(const Fight& F, const Body& x) {
        const MoveClaim* q = StrictClaim(F, x); if (!q || q->order.mode == MV::MoveMode::Free || q->phase == StrictPhase::Ended) return false;
        return !MoveBodyTaken(F, x) && !StrictLeashed(F, x, *q);
    }
    [[nodiscard]] inline bool StrictLive(const Fight& F, const Body& x) { const MoveClaim* q = StrictClaim(F, x); return q && F.now >= q->fearFree && F.now >= q->carriedAt && !q->notCarried && StrictFiled(F, x); }
    // Declared in World.h: the engine's own Flee / Hide are never picked under an order.
    inline bool StrictBars(const Fight& F, const Body& x) { return g_road == Road::Strict && !g_moveFault->fleeWins && StrictFiled(F, x); }
    // Declared in World.h: no attack is started (a Disengage order that travels, a fear that holds his hands).
    inline bool StrictNoAttack(const Fight& F, const Body& x) { const MoveClaim* q = StrictClaim(F, x); if (q && F.now < q->fearFree && !(g_residue.fearActs > 0.0f) && StrictFiled(F, x)) return true; return q && q->order.mode == MV::MoveMode::Disengage && q->phase == StrictPhase::Travelling && !g_moveFault->disengageFights && StrictLive(F, x); }
    [[nodiscard]] inline bool StrictNear(const Body& x, const EQ::Vec3& p) { return std::fabs(p.z - x.c.z) < g_envT.storeyU; }
    [[nodiscard]] inline float StrictDist(const Body& x, const MoveClaim& q) { return StrictNear(x, q.order.p) ? std::hypot(q.order.p.x - x.c.x, q.order.p.y - x.c.y) : std::max(q.order.leashR + 1.0f, std::hypot(q.order.p.x - x.c.x, q.order.p.y - x.c.y)); }

    [[nodiscard]] inline bool StrictHighOff(const Body& x, const MoveClaim& q) { return g_residue.arriveZ > 0.0f && std::fabs(q.order.p.z - x.c.z) > g_residue.arriveZ; }
    [[nodiscard]] inline float StrictStop(const MV::MoveOrder& o) { return std::min(g_residue.arriveError * o.arriveR, o.leashR); }   // the farthest he stops from the point
    inline void StrictPublish(Fight& F, Body& x, MoveClaim& q, const MV::MoveOrder& o) {
        const bool was = q.order.mode != MV::MoveMode::Free && q.phase != StrictPhase::Ended, moving = was && (q.phase == StrictPhase::Travelling || q.phase == StrictPhase::Returning);
        const bool samePoint = was && q.order.p.x == o.p.x && q.order.p.y == o.p.y && q.order.p.z == o.p.z;
        q.order = o; q.orderGen = o.gen; q.blocks = {}; q.nBlocks = 0; q.dueBlock = -1.0;
        if (x.ctl == 0) { ++F.t.strictOrders[static_cast<std::size_t>(o.mode)]; }
        if (!was) q.fearSeen = 0.0;   // a fear that began before the order holds it off (again)
        if (o.mode == MV::MoveMode::Free) { q.phase = StrictPhase::None; return; }
        if (samePoint && q.phase == StrictPhase::Standing && StrictDist(x, q) <= std::max(q.stopAt, o.arriveR)) return;   // only the radii or the mode changed: the stand is not interrupted where he has arrived. Else he travels, as the point was filed anew
        const bool filed = q.filed; q.filed = true;
        q.phase = StrictPhase::Pending; q.stopAt = StrictStop(o) * StrictHash(F, x, 0xA11CEull ^ (static_cast<std::uint64_t>(o.gen) << 20));
        if (!samePoint) {   // the same point in place: what its claim waits for, it waits for
            q.carriedAt = F.now + static_cast<double>(!filed ? g_residue.startDelay : was ? g_residue.refileStall : std::max(g_residue.refileStall, g_residue.startDelay));
            q.notCarried = StrictHash(F, x, 0xCA44ull ^ (static_cast<std::uint64_t>(o.gen) << 20)) < g_residue.notCarriedShare;
            q.notBefore = std::max(q.carriedAt, F.now + static_cast<double>(moving ? g_residue.repointStall : 0.0f)); }
        x.nav.n = x.nav.i = 0;
        if (StrictDist(x, q) > o.arriveR && StrictHash(F, x, 0xB10CCull ^ (static_cast<std::uint64_t>(o.gen) << 20)) < g_residue.blockedShare) q.dueBlock = F.now + static_cast<double>(g_moveT.blockAfterSec);
    }
    // A leg of an order ended without ground, or the residue says so. true = the order is over.
    inline bool StrictBlock(Fight& F, Body& x, MoveClaim& q) {
        if (x.ctl == 0) ++F.t.strictBlocks;
        q.phase = StrictPhase::Blocked; q.retryAt = F.now + static_cast<double>(g_residue.blockBackoff);
        if (!(g_residue.blockTold > 0.0f)) return false;   // he holds and tries again, for ever
        if (q.nBlocks && F.now - q.blocks[0] > static_cast<double>(g_residue.blockWindow)) { std::rotate(q.blocks.begin(), q.blocks.begin() + 1, q.blocks.end()); --q.nBlocks; }
        q.blocks[q.nBlocks++] = F.now;
        if (q.nBlocks < q.blocks.size()) return false;
        if (x.ctl == 0) ++F.t.strictUnreach;
        q.phase = StrictPhase::Ended;
        if (MV::Member* m = MoveMember(F, x.c.id)) { m->marks |= MV::kMarkPathBlocked; m->commit = MV::Commit{}; m->order.mode = MV::MoveMode::Free; }
        return true;
    }

    // The pick, for everybody, at his own phase of the grid.
    inline void StrictStep(Fight& F) {
        for (const Ctl& c : F.ctl) { Body& x = F.b[static_cast<std::size_t>(c.body)]; MoveClaim* q = StrictClaim(F, x); if (!q) continue;
            if (q->nextAt >= 0.0 && F.now >= q->nextAt) { q->nextAt = -1.0; const MV::MoveOrder o = q->next; StrictPublish(F, x, *q, o); }
            if (F.now < x.fearUntil && x.fearUntil != q->fearSeen && StrictFiled(F, x)) { q->fearSeen = x.fearUntil; q->fearFree = std::min(x.fearUntil, F.now + static_cast<double>(g_residue.fearUncarried)); }
            const bool live = Fight::Up(x) && StrictLive(F, x);
            if ((!live || (q->phase != StrictPhase::Travelling && q->phase != StrictPhase::Returning)) && (x.act == Act::Shoot || x.act == Act::Cast)) q->actSeen = x.actEnd;   // begun before he travels: no shotStop
            if (live && x.ctl == 0) { const float d = StrictDist(x, *q); F.t.strictSec += TM::dt; if (d > q->order.leashR && (q->phase == StrictPhase::Standing || q->phase == StrictPhase::Returning)) F.t.strictOutSec += TM::dt; if (q->phase == StrictPhase::Standing || q->phase == StrictPhase::Returning) F.t.strictStandSec += TM::dt; }
            if (F.now < q->nextPick) continue;
            q->nextPick = F.now + static_cast<double>(g_residue.selectorLag) * (q->nextPick <= 0.0 ? 0.25 + 0.75 * static_cast<double>(StrictHash(F, x, 0x91C4ull)) : 1.0);   // his own offset on the grid
            if (!live) { if (q->phase == StrictPhase::Returning && F.now < q->fearFree) continue;   // a walk back a fear interrupts goes on as one
                if (q->phase == StrictPhase::Returning || q->phase == StrictPhase::Standing) q->phase = StrictPhase::Pending; continue; }   // Yielded: back as Travelling or Standing by distance
            const float d = StrictDist(x, *q);
            switch (q->phase) {
            case StrictPhase::Pending:   if (F.now >= q->notBefore) q->phase = d > std::max(q->stopAt, q->order.arriveR) || StrictHighOff(x, *q) ? StrictPhase::Travelling : StrictPhase::Standing; break;
            case StrictPhase::Standing:  if (d > q->order.leashR && !g_moveFault->noReturn) { q->phase = StrictPhase::Returning; q->stopAt = StrictStop(q->order) * StrictHash(F, x, 0x4E7ull ^ static_cast<std::uint64_t>(F.now * 1000.0)); if (x.ctl == 0) ++F.t.strictDisplaced; x.nav.n = x.nav.i = 0; } break;
            case StrictPhase::Blocked:   if (F.now >= q->retryAt) { q->phase = StrictPhase::Travelling; x.nav.n = x.nav.i = 0; } break;
            default: break;
            }
        }
    }

    // His feet under an order. Called once a step, whatever his hands do (the engine's movement half is its own thread of the tree), except under a blow, which roots him.
    inline void StrictFeet(Fight& F, Body& x) {
        MoveClaim* q = StrictClaim(F, x); if (!q || !StrictLive(F, x)) return;
        if (F.now < x.staggerUntil || F.now < x.ctrlUntil || x.airborne || ((x.act == Act::Swing || x.act == Act::Power || x.act == Act::Bash) && F.now < x.actEnd)) return;
        if (q->dueBlock > 0.0 && F.now >= q->dueBlock) { q->dueBlock = F.now + static_cast<double>(g_moveT.blockAgainSec) + static_cast<double>(g_residue.blockBackoff); if (StrictBlock(F, x, *q)) { MoveTrustLeg(F, x, TR::End::Refused, true, q->order.p); MoveRelease(F, x.ctl, "Unreachable (residue)"); } return; }
        if (q->phase == StrictPhase::Standing && g_residue.standDrift > 0.0f) {
            const float a = 6.2831853f * StrictHash(F, x, static_cast<std::uint64_t>(F.now / static_cast<double>(g_moveT.strictDriftSec))), tx = q->order.p.x + g_residue.standDrift * std::cos(a), ty = q->order.p.y + g_residue.standDrift * std::sin(a);
            if (std::hypot(tx - x.c.x, ty - x.c.y) > 10.0f) EnvTryStep(F, x, tx, ty, g_moveT.strictDriftX * x.spec->run * TM::dt); return; }
        if (q->phase != StrictPhase::Travelling && q->phase != StrictPhase::Returning) return;
        if ((x.act == Act::Shoot || x.act == Act::Cast) && x.actEnd != q->actSeen) { q->actSeen = x.actEnd; q->stopUntil = F.now + static_cast<double>(g_residue.shotStop); }   // a shot or a cast begun on the way
        if (F.now < q->stopUntil) return;
        if (StrictNear(x, q->order.p) && StrictDist(x, *q) <= std::max(g_moveT.strictMinStopU, q->stopAt)) {
            if (StrictHighOff(x, *q)) { if (!g_moveFault->noBlock && StrictBlock(F, x, *q)) MoveRelease(F, x.ctl, "Unreachable (height)"); return; }
            q->phase = StrictPhase::Standing; q->dueBlock = -1.0;
            if (!q->arrived) { q->arrived = true; MoveTrustLeg(F, x, TR::End::Arrived, true, q->order.p); q->len = 0.0f; if (x.ctl == 0) ++F.t.spotArrived; if (F.trace) std::fprintf(F.trace, "%7.2f  ORDER ARRIVED %s\n", F.now, x.c.name.c_str()); }
            return; }
        // how fast: along the travel direction, against the foe he faces
        const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; const Body* tg = F.Find(c.steer ? c.steer : x.c.targetId); float speed = x.spec->run;
        const bool freeAim = (F.move->pic.caps.disengage || g_moveFault->gotoRuns) && (q->order.mode == MV::MoveMode::GoTo || q->order.mode == MV::MoveMode::Disengage);
        if (q->phase == StrictPhase::Returning) speed = std::min(speed, g_residue.walkBackSpeed);
        else if (!freeAim && tg && Fight::Up(*tg) && !tg->Party() && !g_moveFault->kiteRuns) {
            EQ::Vec3 to = q->order.p; if (x.nav.i < x.nav.n) to = x.nav.pts[x.nav.i].p;
            const float dx = to.x - x.c.x, dy = to.y - x.c.y, fx = tg->c.x - x.c.x, fy = tg->c.y - x.c.y;
            const float dot = dx * fx + dy * fy, edge = g_moveT.strictAimCos * std::hypot(dx, dy) * std::hypot(fx, fy);
            if (dot < edge) speed = std::min(speed, dot < -edge ? g_residue.kiteSpeed : g_residue.strafeSpeed); }
        x.ranLast = speed >= 0.9f * x.spec->run;
        if (x.sneaking) speed *= g_moveT.sneakX;
        EnvPoint pt; pt.p = q->order.p; pt.tri = q->order.tri;
        if (F.now - q->walkAt > 2.5 * static_cast<double>(TM::dt) && q->walked > 0.0f) q->len = 0.0f;
        const EQ::Vec3 from{ x.c.x, x.c.y, x.c.z }; q->walkAt = F.now; q->walks = true;
        EnvMove(F, x, q->order.p.x, q->order.p.y, speed, false, nullptr, &pt); q->walks = false;
        if (freeAim) Fight::Face(x, q->order.p.x, q->order.p.y);
        const float went = MV::Dist(from, { x.c.x, x.c.y, x.c.z }); q->walked += went;
        if (x.ctl == 0) { F.t.strictTravelSec += TM::dt; F.t.strictTravelU += static_cast<double>(went); }
        for (std::size_t i = 0; i < q->via.size(); ++i) if (!(q->passed & (1u << i)) && MV::Dist(q->via[i], { x.c.x, x.c.y, x.c.z }) < 100.0f) q->passed |= 1u << i;
        if (q->on && q->len > 0.0f && TR::Seen(TR::Sign::LengthFar, q->walked / q->len) && q->walked - q->len >= F.move->pic.trust.rules.lengthFloorU) { MoveTrustLeg(F, x, TR::End::Walking, true, q->order.p); q->len = 0.0f; }
    }

    // A started attack carries him (marth: lunges allowed). Toward the target, never through it, whatever the leash says: the next picks walk him back.
    inline void StrictLunge(Fight& F, Body& x, const Body& tg, bool power) {
        if (!StrictLive(F, x)) return;
        const MoveClaim& q = *StrictClaim(F, x); if (q.order.flags & 1u) return;   // kMove_AttackInPlace
        if (StrictHash(F, x, 0x1A6Eull ^ static_cast<std::uint64_t>(F.now * 1000.0)) >= g_residue.lungeShare) return;
        const float dx = tg.c.x - x.c.x, dy = tg.c.y - x.c.y, l = std::hypot(dx, dy), u = std::min(power ? g_residue.lungeOutPower : g_residue.lungeOut, l - g_moveT.lungeStandoffU);
        if (u <= 1.0f) return;
        EnvShove(F, x, dx / l * u, dy / l * u); x.movedAt = F.now; if (x.ctl == 0) ++F.t.strictLunges;
    }
    // A dodge (dx, dy) under an order ends inside the leash: the engine's own clamp.
    inline void StrictClampDodge(const Fight& F, const Body& x, float& dx, float& dy) {
        if (!StrictLive(F, x) || g_moveFault->dodgeOut) return;
        const MoveClaim& q = *StrictClaim(F, x); if (!g_residue.dodgeInLeash) { dx = dy = 0.0f; return; }
        const float ox = x.c.x - q.order.p.x, oy = x.c.y - q.order.p.y, R = q.order.leashR;
        if (std::hypot(ox + dx, oy + dy) <= R) return;
        // where the segment leaves the disc. Already outside: no step
        const float a = dx * dx + dy * dy, b = 2.0f * (ox * dx + oy * dy), c = ox * ox + oy * oy - R * R, disc = b * b - 4.0f * a * c;
        const float k = a > 0.0f && c <= 0.0f && disc > 0.0f ? std::clamp((-b + std::sqrt(disc)) / (2.0f * a), 0.0f, 1.0f) : 0.0f; dx *= k; dy *= k;
    }
    inline void StrictPushed(Fight& F, Body& x, const Body* by, bool down) {   // a stagger's or a fall's own displacement (0 until it is measured)
        const float u = down ? g_residue.displaceDownU : g_residue.displaceU;
        if (!(u > 0.0f) || !by || !StrictLive(F, x)) return;
        const float dx = x.c.x - by->c.x, dy = x.c.y - by->c.y, l = std::max(1.0f, std::hypot(dx, dy)); EnvShove(F, x, dx / l * u, dy / l * u);
    }
    [[nodiscard]] inline double StrictThinkX(const Fight& F, const Body& x) { const MoveClaim* q = StrictClaim(F, x); return q && (q->order.flags & 1u) && StrictLive(F, x) ? 1.0 / static_cast<double>(std::max(0.05f, g_residue.attackInPlaceRate)) : 1.0; }
}
