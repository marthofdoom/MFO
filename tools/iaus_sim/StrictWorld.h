#pragma once
// iaus_sim/StrictWorld.h -- THE STRICT EXECUTOR (tight-movement-plan.md 11, hm1-seat-brief.md 7 and 15): what Harbinger's strict orders do to a body. HOST-ONLY.
// Included by MoveWorld.h. Nothing here decides: the order is native/iaus MoveOrder (MovePass::Orders), this carries it out (--position-road strict).
//
//   the pick     every Residue::selectorLag seconds (the engine's Movement Selector) he is asked: travel or stand. A crossing of the leash is seen then, not before
//   Travelling   along the sim's own mover to the point, until inside the arrival error. Backing away from the foe he faces he has Residue::kiteSpeed, not his run
//                (Harbinger cannot release the aim before H-M3: RoadCaps::disengage gives GoTo and Disengage his full run). He FIGHTS on the way, except in Disengage
//   Standing     no movement of his own at all. What moves him: a lunge, a dodge (cut at the leash), a shove, a stagger. Past leashR the next pick walks him back
//   Blocked      a leg that ended Blocked or Unreachable: he stands Residue::blockBackoff, then travels again. Three in blockWindow end the order (Unreachable)
//   the engine's Flee and Hide are never picked (marth: our order beats Flee / Hide). MFO's own runs (hide run, back-off, retreat, regroup) keep their precedence
//   until G2's one owner: the order waits under them, as on the point road.
// Free is not here: no order, the engine's own movement model, unchanged.
#include "iaus_sim/World.h"

namespace Sim {

    [[nodiscard]] inline MoveClaim* StrictClaim(Fight& F, const Body& x) { return g_road == Road::Strict && F.move && x.ctl >= 0 && static_cast<std::size_t>(x.ctl) < F.move->claim.size() ? &F.move->claim[static_cast<std::size_t>(x.ctl)] : nullptr; }
    [[nodiscard]] inline const MoveClaim* StrictClaim(const Fight& F, const Body& x) { return StrictClaim(const_cast<Fight&>(F), x); }
    [[nodiscard]] inline float StrictHash(const Fight& F, const Body& x, std::uint64_t salt) { return static_cast<float>(Fight::Mix64(F.avSeed ^ Fight::Mix64(salt ^ (static_cast<std::uint64_t>(x.c.id) << 32))) >> 40) / 16777216.0f; }

    // ch.23: a leash wins over a point. The order is not carried while its point lies past it.
    [[nodiscard]] inline bool StrictLeashed(const Fight& F, const Body& x, const MoveClaim& q) { const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; return c.leash > 0.0f && std::hypot(q.order.p.x - F.b[0].c.x, q.order.p.y - F.b[0].c.y) > c.leash + kLeashStandU; }
    // An order that is live on this body now.
    [[nodiscard]] inline bool StrictLive(const Fight& F, const Body& x) {
        const MoveClaim* q = StrictClaim(F, x); if (!q || q->order.mode == MV::MoveMode::Free || q->phase == StrictPhase::Ended) return false;
        return !MoveBodyTaken(F, x) && !StrictLeashed(F, x, *q);
    }
    // Declared in World.h: the engine's own Flee / Hide are never picked under an order.
    inline bool StrictBars(const Fight& F, const Body& x) { return g_road == Road::Strict && !g_moveFault->fleeWins && StrictLive(F, x); }
    // Declared in World.h: no attack is started (a Disengage order that travels).
    inline bool StrictNoAttack(const Fight& F, const Body& x) { const MoveClaim* q = StrictClaim(F, x); return q && q->order.mode == MV::MoveMode::Disengage && q->phase == StrictPhase::Travelling && !g_moveFault->disengageFights && StrictLive(F, x); }
    [[nodiscard]] inline bool StrictNear(const Body& x, const EQ::Vec3& p) { return std::fabs(p.z - x.c.z) < g_envT.storeyU; }
    [[nodiscard]] inline float StrictDist(const Body& x, const MoveClaim& q) { return StrictNear(x, q.order.p) ? std::hypot(q.order.p.x - x.c.x, q.order.p.y - x.c.y) : std::max(q.order.leashR + 1.0f, std::hypot(q.order.p.x - x.c.x, q.order.p.y - x.c.y)); }

    inline void StrictPublish(Fight& F, Body& x, MoveClaim& q, const MV::MoveOrder& o) {
        const bool was = q.order.mode != MV::MoveMode::Free && q.phase != StrictPhase::Ended, moving = was && (q.phase == StrictPhase::Travelling || q.phase == StrictPhase::Returning);
        const bool samePoint = was && q.order.p.x == o.p.x && q.order.p.y == o.p.y && q.order.p.z == o.p.z;
        q.order = o; q.orderGen = o.gen; q.blocks = {}; q.nBlocks = 0; q.dueBlock = -1.0;
        if (x.ctl == 0) { ++F.t.strictOrders[static_cast<std::size_t>(o.mode)]; }
        if (o.mode == MV::MoveMode::Free) { q.phase = StrictPhase::None; return; }
        if (samePoint && q.phase == StrictPhase::Standing && StrictDist(x, q) <= o.leashR) return;   // only the radii or the mode changed: the stand is not interrupted
        q.phase = StrictPhase::Pending; q.stopAt = g_residue.arriveError * o.arriveR * StrictHash(F, x, 0xA11CEull ^ (static_cast<std::uint64_t>(o.gen) << 20));
        q.notBefore = F.now + (was ? (moving && !samePoint ? static_cast<double>(g_residue.repointStall) : 0.0) : static_cast<double>(g_residue.startDelay));
        x.nav.n = x.nav.i = 0;
        if (StrictDist(x, q) > o.arriveR && StrictHash(F, x, 0xB10CCull ^ (static_cast<std::uint64_t>(o.gen) << 20)) < g_residue.blockedShare) q.dueBlock = F.now + 0.5;
    }
    // A leg of an order ended without ground, or the residue says so. true = the order is over.
    inline bool StrictBlock(Fight& F, Body& x, MoveClaim& q) {
        if (x.ctl == 0) ++F.t.strictBlocks;
        q.phase = StrictPhase::Blocked; q.retryAt = F.now + static_cast<double>(g_residue.blockBackoff);
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
            const bool live = Fight::Up(x) && StrictLive(F, x);
            if (live && x.ctl == 0) { const float d = StrictDist(x, *q); F.t.strictSec += TM::dt; if (d > q->order.leashR && (q->phase == StrictPhase::Standing || q->phase == StrictPhase::Returning)) F.t.strictOutSec += TM::dt; if (q->phase == StrictPhase::Standing || q->phase == StrictPhase::Returning) F.t.strictStandSec += TM::dt; }
            if (F.now < q->nextPick) continue;
            q->nextPick = F.now + static_cast<double>(g_residue.selectorLag) * (q->nextPick <= 0.0 ? 0.25 + 0.75 * static_cast<double>(StrictHash(F, x, 0x91C4ull)) : 1.0);   // his own offset on the grid
            if (!live) { if (q->phase == StrictPhase::Returning || q->phase == StrictPhase::Standing) q->phase = StrictPhase::Pending; continue; }   // Yielded: back as Travelling or Standing by distance
            const float d = StrictDist(x, *q);
            switch (q->phase) {
            case StrictPhase::Pending:   if (F.now >= q->notBefore) q->phase = d > std::max(q->stopAt, q->order.arriveR) ? StrictPhase::Travelling : StrictPhase::Standing; break;
            case StrictPhase::Standing:  if (d > q->order.leashR && !g_moveFault->noReturn) { q->phase = StrictPhase::Returning; q->stopAt = g_residue.arriveError * q->order.arriveR * StrictHash(F, x, 0x4E7ull ^ static_cast<std::uint64_t>(F.now * 1000.0)); if (x.ctl == 0) ++F.t.strictDisplaced; x.nav.n = x.nav.i = 0; } break;
            case StrictPhase::Blocked:   if (F.now >= q->retryAt) { q->phase = StrictPhase::Travelling; x.nav.n = x.nav.i = 0; } break;
            default: break;
            }
        }
    }

    // His feet under an order. Called once a step, whatever his hands do (the engine's movement half is its own thread of the tree), except under a blow, which roots him.
    inline void StrictFeet(Fight& F, Body& x) {
        MoveClaim* q = StrictClaim(F, x); if (!q || !StrictLive(F, x)) return;
        if (F.now < x.staggerUntil || F.now < x.ctrlUntil || x.airborne || ((x.act == Act::Swing || x.act == Act::Power || x.act == Act::Bash) && F.now < x.actEnd)) return;
        if (q->dueBlock > 0.0 && F.now >= q->dueBlock) { q->dueBlock = F.now + 1.0 + static_cast<double>(g_residue.blockBackoff); if (StrictBlock(F, x, *q)) { MoveTrustLeg(F, x, TR::End::Refused, true, q->order.p); MoveRelease(F, x.ctl, "Unreachable (residue)"); } return; }
        if (q->phase == StrictPhase::Standing && g_residue.standDrift > 0.0f) {
            const float a = 6.2831853f * StrictHash(F, x, static_cast<std::uint64_t>(F.now / 3.0)), tx = q->order.p.x + g_residue.standDrift * std::cos(a), ty = q->order.p.y + g_residue.standDrift * std::sin(a);
            if (std::hypot(tx - x.c.x, ty - x.c.y) > 10.0f) EnvTryStep(F, x, tx, ty, 0.3f * x.spec->run * TM::dt); return; }
        if (q->phase != StrictPhase::Travelling && q->phase != StrictPhase::Returning) return;
        if (StrictNear(x, q->order.p) && StrictDist(x, *q) <= std::max(6.0f, q->stopAt)) {
            q->phase = StrictPhase::Standing; q->dueBlock = -1.0;
            if (!q->arrived) { q->arrived = true; MoveTrustLeg(F, x, TR::End::Arrived, true, q->order.p); q->len = 0.0f; if (x.ctl == 0) ++F.t.spotArrived; if (F.trace) std::fprintf(F.trace, "%7.2f  ORDER ARRIVED %s\n", F.now, x.c.name.c_str()); }
            return; }
        // how fast: along the travel direction, against the foe he faces
        const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; const Body* tg = F.Find(c.steer ? c.steer : x.c.targetId); float speed = x.spec->run;
        const bool freeAim = F.move->pic.caps.disengage && (q->order.mode == MV::MoveMode::GoTo || q->order.mode == MV::MoveMode::Disengage);
        if (!freeAim && tg && Fight::Up(*tg) && !tg->Party() && !g_moveFault->kiteRuns) {
            EQ::Vec3 to = q->order.p; if (x.nav.i < x.nav.n) to = x.nav.pts[x.nav.i].p;
            const float dx = to.x - x.c.x, dy = to.y - x.c.y, fx = tg->c.x - x.c.x, fy = tg->c.y - x.c.y;
            if (dx * fx + dy * fy < 0.0f) speed = std::min(speed, g_residue.kiteSpeed); }
        x.ranLast = speed >= 0.9f * x.spec->run;
        if (x.sneaking) speed *= 0.55f;
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
        const float dx = tg.c.x - x.c.x, dy = tg.c.y - x.c.y, l = std::hypot(dx, dy), u = std::min(power ? g_residue.lungeOutPower : g_residue.lungeOut, l - 60.0f);
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
    inline void StrictPushed(Fight& F, Body& x, const Body* by) {   // a stagger's own displacement (Residue::displaceU, 0 until it is measured)
        if (!(g_residue.displaceU > 0.0f) || !by || !StrictLive(F, x)) return;
        const float dx = x.c.x - by->c.x, dy = x.c.y - by->c.y, l = std::max(1.0f, std::hypot(dx, dy)); EnvShove(F, x, dx / l * g_residue.displaceU, dy / l * g_residue.displaceU);
    }
    [[nodiscard]] inline double StrictThinkX(const Fight& F, const Body& x) { const MoveClaim* q = StrictClaim(F, x); return q && (q->order.flags & 1u) && StrictLive(F, x) ? 1.0 / static_cast<double>(std::max(0.05f, g_residue.attackInPlaceRate)) : 1.0; }
}
