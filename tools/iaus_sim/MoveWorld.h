#pragma once
// iaus_sim/MoveWorld.h -- SIM V4, S2: the movement pass in the sim's world (Docs/research/2026-11/movement-plan.md S2, iaus-sim-v4.md 8.7). HOST-ONLY.
// Included at the end of World.h, after EnvWorld.h. Nothing here decides anything: the decision is native/iaus MovePass, compiled in.
//
// ONE WINDOW (MoveDecisionSec, never under 0.5 s)
//   its start   the picture is filled from the fight and frozen, phase 1 runs in one go (frozen inputs make it the same however it is sliced)
//   across it   the ray asks are answered at an even rate, each by the TRUE geometry at the moment it is cast, stamped with that moment
//   its end     phase 3 on the answers, Adopt, and the claims change. So a fact is at most a window old when it is used, as in the game
// An event (hit hard, a leg that ended, the leash) sets a mark. It never starts a window.
//
// TRUST (S3): the claim's legs tell EnvTrust how they ended (MoveTrustLeg: no ground, refused, the engine's way far from ours), every party body's walk clears
// marks (MoveTrustWalk), and the [env-trust] lines go to the trace and MoveState::trustLog. The pass reads the table, nothing here decides by it.
//
// THE CLAIM (ch.24 point + hold, MoveTuning): he walks to the point by the sim's own mover (EnvMove: the same routes, doors, bodies, leg ends), fights a foe
// in reach on the way, and then holds: his own combat movement does not leave the radius. Retreat, flee, a run of card G3 / batch I, a move order and the
// hold row keep their precedence (today's rules, until G2's one owner): the claim waits under them and its member is not asked.
#include "iaus_sim/World.h"

namespace Sim {

    inline thread_local const Fight* tl_moveFight = nullptr;   // ClassProfiles is a plain function: the profile of Fight::ctl[classId]
    [[nodiscard]] inline const Profile& MoveProfile(std::uint8_t ci) { static const Profile none; return tl_moveFight && ci < tl_moveFight->ctl.size() ? tl_moveFight->ctl[ci].profile : none; }

    // Somebody else has his body (the precedence above), or he cannot act.
    [[nodiscard]] inline bool MoveBodyTaken(const Fight& F, const Body& x) {
        const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)];
        return !Fight::Up(x) || c.runPhase != 0 || (F.now < x.fleeUntil && (g_road != Road::Strict || g_moveFault->fleeWins)) || F.now < x.retreatUntil || F.now < x.holdUntil || F.now < c.moveUntil || x.airborne || x.perch;
    }
    [[nodiscard]] inline MV::Member* MoveMember(Fight& F, std::uint32_t id) { for (MV::Member& m : F.move->pic.party) if (m.id == id) return &m; return nullptr; }
    inline float MoveRadius() { return g_road == Road::Pinned ? g_moveT.pinRadiusU : g_moveT.radiusU; }

    inline void StrictPublish(Fight& F, Body& x, MoveClaim& q, const MV::MoveOrder& o); inline bool StrictBlock(Fight& F, Body& x, MoveClaim& q); inline void StrictStep(Fight& F); [[nodiscard]] inline bool StrictLive(const Fight& F, const Body& x);   // StrictWorld.h
    inline void MoveRelease(Fight& F, int ci, const char* why) {
        MoveClaim& q = F.move->claim[static_cast<std::size_t>(ci)]; if (!q.on) return;
        q.on = q.arrived = false; if (ci == 0) ++F.t.spotReleased;
        Body& x = F.b[static_cast<std::size_t>(F.ctl[static_cast<std::size_t>(ci)].body)]; x.nav.n = x.nav.i = 0;
        if (F.trace) std::fprintf(F.trace, "%7.2f  SPOT RELEASED %s: %s\n", F.now, x.c.name.c_str(), why);
    }

    inline std::string MoveKindNames(std::uint32_t mask) { std::string o; for (const MV::SpotKindRow& r : MV::kSpotKinds) if (mask & MV::KindBit(r.kind)) o += std::string(o.empty() ? "" : "+") + r.name; return o; }

    inline void MoveTrustLeg(Fight& F, const Body& x, TR::End end, bool laneClear, const EQ::Vec3& toward) {
        MoveState& S = *F.move.s; MoveClaim& q = S.claim[static_cast<std::size_t>(x.ctl)];
        TR::LegEvent e; e.who = x.c.id; e.spot = q.key; e.end = end; e.t = F.now; e.laneClear = laneClear; e.at = { x.c.x, x.c.y, x.c.z }; e.toward = toward;
        e.leg = q.leg; e.planned = q.len; e.walked = q.walked; e.route = q.via; e.passed = q.passed;
        F.t.trustMarks += TR::Leg(S.pic.trust, e);
        if (end != TR::End::Walking) q.leg = ++S.legs;   // an end ends the leg: the claim's next try is another one
    }
    // Nothing that explains a leg without ground: no body within arm's reach ahead of him, no door in hand.
    inline bool MoveLaneClear(const Fight& F, const Body& x, const EQ::Vec3& toward) {
        if (x.nav.door >= 0) return false;
        const float dx = toward.x - x.c.x, dy = toward.y - x.c.y, l = std::max(1.0f, std::hypot(dx, dy));
        for (const Body& o : F.b) { if (&o == &x || !Fight::Up(o) || std::fabs(o.c.z - x.c.z) > 100.0f) continue;
            const float ox = o.c.x - x.c.x, oy = o.c.y - x.c.y; if (std::hypot(ox, oy) < 120.0f && (ox * dx + oy * dy) / l > 0.0f) return false; }
        return true;
    }
    // One step of everybody's walk (native: the tick's own samples).
    inline void MoveTrustWalk(Fight& F) {
        MoveState& S = *F.move.s; const std::size_t n0 = S.lastAt.size(); S.lastAt.resize(F.b.size());
        for (std::size_t i = 0; i < F.b.size(); ++i) { const Body& x = F.b[i]; const EQ::Vec3 at{ x.c.x, x.c.y, x.c.z };
            if (i < n0 && x.Party() && Fight::Up(x) && x.role != Role::Summon && (at.x != S.lastAt[i].x || at.y != S.lastAt[i].y) && MV::Dist(S.lastAt[i], at) < 100.0f) { if (g_moveFault->noWalked) { const auto was = S.pic.trust.marks; TR::Walked(S.pic.trust, i == 0, S.lastAt[i], at, F.now); S.pic.trust.marks = was; } else TR::Walked(S.pic.trust, i == 0, S.lastAt[i], at, F.now); }
            S.lastAt[i] = at; }
        for (const TR::Line& l : S.pic.trust.log) { S.trustLog.push_back(l); if (F.trace) std::fprintf(F.trace, "%7.2f  %s\n", F.now, TR::Text(l, F.env->name.c_str()).c_str()); }
        S.pic.trust.log.clear();
    }

    // The picture's inputs, from the fight as it stands. A member keeps what the pass remembers of him (commit, marks).
    inline void MoveFill(Fight& F) {
        MoveState& S = *F.move.s; MV::PartyPicture& P = S.pic;
        P.t = F.now; P.caps = MV::RoadCaps{}; P.caps.pin = P.caps.facing = g_road == Road::Pinned; if (g_road == Road::Strict) { P.caps.strict = P.caps.pin = P.caps.facing = true; P.caps.kite = g_moveCaps.kite; P.caps.disengage = g_moveCaps.disengage; }
        std::vector<MV::Member> was; was.swap(P.party); P.foes.clear(); P.areas.clear();
        for (const Body& x : F.b) {
            if (!Fight::Up(x)) continue;
            if (!x.Party()) {
                MV::Foe f; f.id = x.c.id; f.p = { x.c.x, x.c.y, x.c.z }; const Body* tg = F.Find(x.c.targetId); f.target = tg && tg->Party() && Fight::Up(*tg) ? tg->c.id : 0;
                f.faceX = x.hx; f.faceY = x.hy; if (g_v4.light) f.skill = x.spec->Skill(15);
                if (g_road == Road::Strict) { f.reach = F.Reach(x); f.speed = x.spec->run; bool far = x.spec->ranged.Has(); for (const int i : x.spec->spells) far = far || F.D->foeSpells[static_cast<std::size_t>(i)].e.nature == MFO::Iaus::Prune::Nature::Offense; f.ranged = far ? Tri::Yes : Tri::No; }   // S5-A: the hold's inputs
                P.foes.push_back(f); continue; }
            MV::Member m; for (MV::Member& o : was) if (o.id == x.c.id) { m = o; break; }
            m.id = x.c.id; m.p = { x.c.x, x.c.y, x.c.z }; m.who = WalkerOf(x); m.speed = x.spec->run * (x.sneaking ? g_moveT.sneakX : 1.0f); m.nTop = 0; m.decides = false;
            m.anchor = { F.b[0].c.x, F.b[0].c.y, F.b[0].c.z }; m.leash = 0.0f; m.anchorMoves = S.playerSeen > 0.0 && F.now > S.playerSeen; m.anchorVel = {}; if (m.anchorMoves) { const float dt = static_cast<float>(F.now - S.playerSeen); m.anchorVel = { (m.anchor.x - S.playerWas.x) / dt, (m.anchor.y - S.playerWas.y) / dt, (m.anchor.z - S.playerWas.z) / dt }; }
            if (x.ctl >= 0) { const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; MoveClaim& q = S.claim[static_cast<std::size_t>(x.ctl)];
                m.leash = c.leash; m.classId = static_cast<std::uint8_t>(x.ctl); m.kinds = MV::SpotKindsFor(c.gate) & ~g_moveFault->kinds; m.decides = !MoveBodyTaken(F, x);
                if (S.force != Force::None && x.ctl == 0 && !(S.force == Force::Stand && S.forceVar == 1)) m.kinds = MV::KindBit(MFO::Iaus::SpotKind::Stay); else if (S.force == Force::Stand && x.ctl == 0) m.kinds &= ~(MV::KindBit(MFO::Iaus::SpotKind::Kite) | MV::KindBit(MFO::Iaus::SpotKind::RetreatExit) | MV::KindBit(MFO::Iaus::SpotKind::Regroup) | MV::KindBit(MFO::Iaus::SpotKind::Flank));
                m.top = q.top; m.nTop = q.nTop; if (g_moveFault->meleeHands) for (auto& a : m.top) a.melee = a.target != 0; m.ageSec = static_cast<float>(F.now - q.topAt);
                // what the holder finds: he was hit hard, and a place the leash no longer lets him stand on (a leash wins over a point, kLeashStandU past it he still stands)
                if (x.burstMax > q.burstSeen + 1.0f && x.burstMax >= kOneHitLine * x.c.hpMax) m.marks |= MV::kMarkHitHard;
                q.burstSeen = x.burstMax;
                if (q.pick.on && q.pick.spot && !q.pick.fresh && F.now >= q.pick.until && !g_moveFault->noPickEnd) { q.pick.on = false; m.marks |= MV::kMarkOrder; }   // S5-C: the order's time is over, he is decided for again
                m.wantsDark = g_v4.light && (F.WantsDarkNow(x) || c.gate.darkClass); m.invisible = F.now < x.cloakUntil;
                m.sneakSkill = g_v4.light ? x.spec->Skill(15) + StealthCombine(F.D->sneakSkillRows, *x.spec, false) : -1.0f;
                if (m.commit.on && m.leash > 0.0f && MV::Dist(m.commit.p, m.anchor) > m.leash + kLeashStandU) m.marks |= MV::kMarkOrder;
                if (!m.decides && m.commit.on) { m.commit = MV::Commit{}; }   // another mover has him: the place is given up, he asks again when he is free
            } else m.commit = MV::Commit{};
            P.party.push_back(m);
        }
        S.playerWas = { F.b[0].c.x, F.b[0].c.y, F.b[0].c.z }; S.playerSeen = F.now;   // his walk over the last window
        P.light.clear();
        if (g_v4.light) {   // S4: every actor of the fight is a light meter
            P.exterior = !F.interior; P.detect = F.dg;
            if (F.now >= S.meterAt) { S.meterAt = F.now + static_cast<double>(g_lightT.meterSec);
                for (const Body& x : F.b) if (Fight::Up(x)) { const EQ::Vec3 p{ x.c.x, x.c.y, x.c.z }; MFO::Iaus::Light::Add(P.lightField, { p, g_lightScale * F.LightOn(p, x.nav.tri), F.now, MFO::Iaus::Light::Source::Actor }); } }
        }
        for (const Area& a : F.areas) { const Body* o = F.Find(a.src); if (a.hostile && o && !o->Party()) P.areas.push_back({ { a.x, a.y, a.z }, a.r }); }
        for (const Hazard& h : F.hazards) if (!h.party && !h.pcOnly) P.areas.push_back({ { h.x, h.y, h.z }, h.r });   // 16.3: a hazard is a spot input, as designed
        if (g_moveFault->noTrust) { P.trust.marks.clear(); ++P.trust.gen; } if (g_moveFault->hairTrust) P.trust.rules.repeat = 1; if (g_moveFault->noMemory) P.trust.refused.clear();
        P.sight = g_sightT; if (g_moveFault->terrainBlind) P.sight.terrain = false; if (g_moveFault->blind) P.sight.walls = false;
    }

    inline void MoveOpen(Fight& F) {
        MoveState& S = *F.move.s; const auto t0 = std::chrono::steady_clock::now();
        MoveFill(F); tl_moveFight = &F;
        Tuning t = *ActiveTuning(); if (g_moveFault->noExits) t.spotExitU = 0.0f;
        const EQ::View v = EnvViewOf(F, true);
        MP::Freeze(S.pic, v, t);
        if (g_moveFault->reachBlind || g_moveFault->noPortals) {
            while (!MP::Phase1(S.pic, v, tl_envScratch, &MoveProfile, t, S.asks, 0.0f)) { if (g_moveFault->reachBlind) for (auto& r : S.pic.work.reach) r.second = Tri::Yes; }
            if (g_moveFault->noPortals) { for (MV::Spot& s : S.pic.pool) s.portals = {}; for (auto& p : S.pic.work.pairs) p.path.nPortals = 0; for (MV::Member& m : S.pic.party) m.portals = {}; }
        } else while (!MP::Phase1(S.pic, v, tl_envScratch, &MoveProfile, t, S.asks, MFO::Iaus::kInf)) {}
        if (g_v4.light && g_lightT.sampler && std::any_of(S.pic.party.begin(), S.pic.party.end(), [](const MV::Member& m) { return m.wantsDark; }))   // S4: the point sampler (probe L1), so many spots a window
            for (std::size_t i = 0; i < S.pic.pool.size() && S.pic.light.size() < static_cast<std::size_t>(g_lightT.samples); ++i) { const MV::Spot& s = S.pic.pool[i]; S.pic.light.push_back({ s.key, g_lightScale * F.LightOn(s.p, s.tri) }); }
        S.asked = 0; S.open = true; S.winStart = F.now; S.winEnd = F.now + static_cast<double>(MFO::Iaus::MoveDecisionSec(t));
        const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        g_moveNs += ns; ++g_moveWindows; long long w = g_moveWorstNs.load(); while (ns > w && !g_moveWorstNs.compare_exchange_weak(w, ns)) {}
        ++F.t.moveWindows;
    }

    // The rays of the window, as many as its elapsed share owes (native: the main thread's batch spread over the frames). The TRUE geometry answers,
    // three rays a fact as Sightline::PointSeenNow casts them (viewer's eye to the point's feet, torso and head).
    inline void MoveRays(Fight& F) {
        MoveState& S = *F.move.s; if (!S.open || S.asked >= S.asks.size()) return;
        const double share = std::clamp((F.now + static_cast<double>(TM::dt) - S.winStart) / std::max(1e-6, S.winEnd - S.winStart), 0.0, 1.0);
        const std::size_t due = std::min(S.asks.size(), static_cast<std::size_t>(std::ceil(share * static_cast<double>(S.asks.size()))));
        if (g_moveFault->noRays) { S.asked = due; return; }
        const auto t0 = std::chrono::steady_clock::now(); std::vector<MV::RayAnswer> out; const EnvTuning& T = g_envT; const EQ::View v = EnvViewOf(F);
        EQ::SightTuning st = g_sightT; if (g_moveFault->terrainBlind) st.terrain = false; if (g_moveFault->blind) st.walls = false;
        for (; S.asked < due; ++S.asked) { const MV::RayAsk& a = S.asks[S.asked]; const Body* o = F.Find(a.viewer); if (!o || !Fight::Up(*o)) continue;
            const EQ::Vec3 eye{ o->c.x, o->c.y, o->c.z + T.eyeFrac * T.bodyH }; bool sees = !g_v4.los;
            for (const float h : { T.feetU, T.torsoFrac * T.bodyH, T.eyeFrac * T.bodyH }) { if (sees) break; const EQ::Vec3 to{ a.p.x, a.p.y, a.p.z + h };
                sees = MV::Dist(eye, to) <= T.targetMargin || EQ::CastRay(v, eye, to, tl_envScratch, st, T.targetMargin).clear; }
            out.push_back({ a.spot, a.viewer, sees ? Tri::Yes : Tri::No, F.now, { o->c.x, o->c.y, o->c.z } }); }
        F.t.moveRays += static_cast<long>(out.size()); g_moveRays += static_cast<long long>(out.size());
        MP::Answer(S.pic, out, *ActiveTuning());
        g_moveNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }

    // The action he is on, first in his ranking (S5-C): what the oracle chose, and what an order being tried is paired with.
    inline void MoveNoteFront(MoveClaim& q, MFO::Iaus::RankedAction a) {
        if (a.family == Family::kCount) return;
        const auto same = [&](const MFO::Iaus::RankedAction& r) { return r.family == a.family && r.form == a.form && r.target == a.target && r.moveOnly == a.moveOnly; };
        const auto e = q.top.begin() + q.nTop; auto i = std::find_if(q.top.begin(), e, same);
        if (i == e) { if (q.nTop < q.top.size()) ++q.nTop; i = q.top.begin() + (q.nTop - 1); *i = a; }   // not ranked: the last row makes room
        std::rotate(q.top.begin(), i, i + 1);
    }
    // ── S5-C: orders that are not the pass's. PicksBefore makes each such member a fixed one, so the same assignment table plans the others around him. PicksAfter
    // takes the borrowed row of a member on his own place out again and says what his assignment is.
    inline void PicksBefore(Fight& F) {
        MoveState& S = *F.move.s; MV::PartyPicture& P = S.pic;
        for (std::size_t ci = 0; ci < S.claim.size(); ++ci) { MoveClaim& q = S.claim[ci]; OrderPick& k = q.pick; if (!k.on) continue;
            MV::Member* m = MoveMember(F, F.b[static_cast<std::size_t>(F.ctl[ci].body)].c.id);
            if (!m || !m->decides || m->tri < 0 || (S.force != Force::None && ci == 0)) { k.on = false; continue; }
            if (!k.fresh) {
                if (k.spot) { if (!m->commit.on || m->commit.key != k.key) k.on = false; continue; }   // the commit stands by itself
                if (F.now >= k.until - 1e-9 || m->seen) { k.on = false; continue; } }
            if (k.spot) {
                const auto s = std::find_if(P.pool.begin(), P.pool.end(), [&](const MV::Spot& x) { return x.key == k.key; });
                if (s == P.pool.end()) { k.on = false; continue; }
                const double at = m->commit.on && m->commit.key == k.key ? m->commit.at : P.t;
                m->commit = MV::Commit{ true, k.key, s->p, s->tri, k.kinds, k.act, k.act.family == Family::kCount ? m->top[0] : k.act, at };
            } else {
                MV::Spot s; s.key = MV::KeyOfStay(m->id); s.p = m->p; s.tri = m->tri; s.kinds = k.kinds; s.portals = m->portals;   // the stay keys sort behind every place: no index moves
                P.pool.insert(std::upper_bound(P.pool.begin(), P.pool.end(), s.key, [](MV::SpotKey a, const MV::Spot& b) { return a < b.key; }), s);
                m->commit = MV::Commit{ true, s.key, s.p, s.tri, k.kinds, k.act, k.act, P.t };
            }
            m->fixed = true;
            if (k.fresh) MoveNoteFront(q, k.act);
        }
    }
    inline void PicksAfter(Fight& F, std::vector<MV::Assignment>& as) {
        MoveState& S = *F.move.s;
        for (MoveClaim& q : S.claim) { if (!q.pick.on) continue; q.pick.fresh = false; if (q.pick.spot) continue;
            MV::Member* m = MoveMember(F, F.b[static_cast<std::size_t>(F.ctl[static_cast<std::size_t>(&q - S.claim.data())].body)].c.id); m->commit = MV::Commit{}; m->fixed = false;
            for (MV::Assignment& a : as) if (a.who == m->id) { a.spot = -1; a.key = MV::KeyOfStay(m->id); a.p = m->p; a.tri = m->tri; a.kinds = q.pick.kinds; a.kept = q.pick.kinds == MV::KindBit(MFO::Iaus::SpotKind::Hold) && MV::HoldPoint(*m, a.p, a.tri); } }
        while (!S.pic.pool.empty() && (S.pic.pool.back().key & MV::kStayBit)) S.pic.pool.pop_back();
    }

    inline void MoveClose(Fight& F) {
        MoveState& S = *F.move.s; const auto t0 = std::chrono::steady_clock::now(); tl_moveFight = &F;
        std::vector<MV::Assignment> as; PicksBefore(F); MP::Phase3(S.pic, &MoveProfile, *ActiveTuning(), as); PicksAfter(F, as); MP::Adopt(S.pic, as);
        g_moveNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        S.last = as;
        if (g_road == Road::Strict) {
            std::vector<MV::MoveOrder> was; for (const MV::Member& m : S.pic.party) was.push_back(m.order);
            MP::Orders(S.pic, as, *ActiveTuning());
            for (MV::Member& m : S.pic.party) { Body* x = F.Find(m.id); if (!x || x->ctl < 0) continue; MoveClaim& q = S.claim[static_cast<std::size_t>(x->ctl)];
                if (q.pick.on && q.pick.spot && m.commit.on && m.commit.key == q.pick.key && m.order.mode != q.pick.mode && m.order.mode != MV::MoveMode::Free) { const MV::MoveOrder& w = was[static_cast<std::size_t>(&m - S.pic.party.data())]; m.order.mode = q.pick.mode; m.order.gen = w.gen + (m.order.Same(w) ? 0u : 1u); }   // S5-C: the place, in the mode it was given
                MV::MoveOrder o = m.order; if (S.force != Force::None && x->ctl == 0) { S.forced = o; if (S.forceRule) S.forceRule(F, S.forced); if (S.forced.mode != MV::MoveMode::Free && S.forced.mode != MV::MoveMode::kCount) ++S.forceUsed; o = S.forced; const MV::MoveOrder& stands = q.nextAt >= 0.0 ? q.next : q.order; o.gen = S.forceGen + (o.Same(stands) && (o.mode == MV::MoveMode::Free || q.phase != StrictPhase::Ended) ? 0u : 1u); S.forceGen = o.gen; }   // an order that ended is filed again when its rule fires again (F9)
                if (g_moveFault->noOrders) { o = MV::MoveOrder{}; o.gen = m.order.gen; }
                if (o.gen != (q.nextAt >= 0.0 ? q.next.gen : q.orderGen)) { if (g_residue.bridgeLag > 0.0f) { q.next = o; q.nextAt = F.now + static_cast<double>(g_residue.bridgeLag); } else StrictPublish(F, *x, q, o); if (F.trace) std::fprintf(F.trace, "%7.2f  ORDER %s: mode %d at (%.0f, %.0f, %.0f) leash %.0f, %.0f from him\n", F.now, x->c.name.c_str(), static_cast<int>(o.mode), o.p.x, o.p.y, o.p.z, o.leashR, std::hypot(o.p.x - x->c.x, o.p.y - x->c.y)); } } }
        const MV::MoveCounts& k = S.pic.counts; Tally& t = F.t;
        t.moveUnfinished += k.unfinished; t.movePathShort += k.pathsSkipped; t.moveAskShort += k.asksShort; t.moveSpotShort += k.spotsDropped; t.moveTripleShort += k.triplesDropped; t.movePaths += k.paths;
        for (const MV::Assignment& a : as) {
            Body* x = F.Find(a.who); if (!x || x->ctl < 0) continue; const int ci = x->ctl; const bool subj = ci == 0; MoveClaim& q = S.claim[static_cast<std::size_t>(ci)];
            const MV::Member* m = MoveMember(F, a.who); if (!m) continue;
            if (!m->commit.on && g_moveFault->stayHolds && m->nTop && !MoveBodyTaken(F, *x)) { if (!q.on) { q.on = q.arrived = true; q.p = { x->c.x, x->c.y, x->c.z }; q.tri = x->nav.tri; q.key = MV::KeyOfStay(a.who); } continue; }
            if (!m->commit.on) { MoveRelease(F, ci, "the pass says: stay"); continue; }
            if (MoveBodyTaken(F, *x)) { MoveRelease(F, ci, "another mover has him"); continue; }
            if (q.on && q.key == m->commit.key) { q.act = m->commit.act; q.kinds = m->commit.kinds; continue; }
            if (subj) { ++t.spotFiled; if (q.on && !q.arrived) ++t.spotChanges; for (std::size_t i = 0; i < MFO::Iaus::kSpotKindCount; ++i) if (m->commit.kinds & (1u << i)) ++t.spotKind[i];
                for (const auto& p : S.pic.work.pairs) if (p.spot == a.spot && S.pic.party[p.member].id == a.who) t.spotPathLen += static_cast<double>(p.path.length); }
            const MP::PathFact* way = nullptr; for (const auto& p : S.pic.work.pairs) if (p.spot == a.spot && S.pic.party[p.member].id == a.who) way = &p.path;
            const float bs = q.burstSeen; const auto top = q.top; const auto nTop = q.nTop; const double topAt = q.topAt; { MoveClaim n; static_cast<StrictHand&>(n) = q; q = n; } q.burstSeen = bs; q.top = top; q.nTop = nTop; q.topAt = topAt;
            q.on = true; q.key = m->commit.key; q.p = m->commit.p; q.tri = m->commit.tri; q.kinds = m->commit.kinds; q.act = m->commit.act; q.since = F.now; q.leg = ++S.legs;
            q.from = { x->c.x, x->c.y, x->c.z };
            if (way) { q.len = way->length; q.via.assign(way->via.begin(), way->via.begin() + way->nVia); }
            x->nav.n = x->nav.i = 0;   // a new claim is a new path
            if (g_moveT.dropShare > 0.0f && g_road != Road::Strict) q.dropDue = static_cast<double>(Fight::Mix64(F.avSeed ^ Fight::Mix64(q.key ^ (static_cast<std::uint64_t>(a.who) << 40)) ^ static_cast<std::uint64_t>(S.pic.window)) >> 11) * (1.0 / 9007199254740992.0) < static_cast<double>(g_moveT.dropShare);
            if (F.trace) std::fprintf(F.trace, "%7.2f  SPOT %s: %s at (%.0f, %.0f, %.0f) for %s -> %u, %.0f from him, score %.3f\n", F.now, x->c.name.c_str(), MoveKindNames(q.kinds).c_str(), q.p.x, q.p.y, q.p.z, std::string(Name(q.act.family)).c_str(), q.act.target, std::hypot(q.p.x - x->c.x, q.p.y - x->c.y), a.score);
            if (F.trace && std::getenv("SIM_WHY")) { const Body* tb = F.Find(q.act.target); const auto it = S.pic.facts.find({ q.key, q.act.target }); std::fprintf(F.trace, "           target at (%.0f, %.0f, %.0f), its look at the place: %s, geometry %d, asked %zu short %u\n", tb ? tb->c.x : 0, tb ? tb->c.y : 0, tb ? tb->c.z : 0, it == S.pic.facts.end() ? "unanswered" : it->second.sees == Tri::Yes ? "yes" : "no", tb ? MV::GeomSees(EnvViewOf(F, true), { tb->c.x, tb->c.y, tb->c.z }, q.p, *ActiveTuning(), tl_envScratch, S.pic.sight) : -1, S.asks.size(), S.pic.counts.asksShort);
                const auto po = MV::PortalsAt(EnvViewOf(F, true), S.pic.wallVert, q.tri, q.p, *ActiveTuning()); std::fprintf(F.trace, "           doorways %llx %llx\n", static_cast<unsigned long long>(po[0]), static_cast<unsigned long long>(po[1]));
                for (const MV::Foe& f : S.pic.foes) std::fprintf(F.trace, "           foe %u at (%.0f, %.0f, %.0f) on %u\n", f.id, f.p.x, f.p.y, f.p.z, f.target); }
        }
        // whom the pass does not decide for any more (down, or another mover has him) holds no claim
        for (std::size_t ci = 0; ci < F.ctl.size(); ++ci) { const Body& x = F.b[static_cast<std::size_t>(F.ctl[ci].body)]; if (S.claim[ci].on && MoveBodyTaken(F, x)) MoveRelease(F, static_cast<int>(ci), "he is not free"); }
        S.open = false;
    }

    inline void MoveStep(Fight& F) {
        if (g_road != Road::Off) {
            if (!F.move) { F.move.s = std::make_unique<MoveState>(); F.move->claim.resize(F.ctl.size()); ++g_moveFights; }
            MoveState& S = *F.move.s; if (S.claim.size() < F.ctl.size()) S.claim.resize(F.ctl.size());
            if (S.open && F.now >= S.winEnd - 1e-9) MoveClose(F);
            if (!S.open) MoveOpen(F);
            MoveRays(F);
            if (g_road == Road::Strict) StrictStep(F);
            MoveTrustWalk(F);
        }
        if (F.ctl.empty()) return;
        const Body& s0 = F.b[static_cast<std::size_t>(F.ctl[0].body)]; if (!Fight::Up(s0)) return;
        if (F.move && F.move->claim[0].on) { F.t.spotClaimSec += TM::dt; if (!F.move->claim[0].arrived) F.t.spotWalkSec += TM::dt; }
        if (g_spotMeter && std::floor(F.now * 2.0) != std::floor((F.now - static_cast<double>(TM::dt)) * 2.0)) {   // each half second: who that targets him sees him
            int n = 0, seen = 0; for (const Body& o : F.b) if (!o.Party() && Fight::Up(o) && o.c.targetId == s0.c.id) { ++n; seen += EnvSees(F, o, s0); }
            if (n) { F.t.threatSec += 0.5; if (!seen) F.t.coverSec += 0.5; F.t.exposedSec += 0.5 * double(seen) / double(n); } }
    }

    // His own lap's ranking, for the pass (the held action first). Called by the policy after Decide.
    inline void MoveNoteRanked(Fight& F, int ci, const Decision& d, const MFO::Iaus::Generated& g, const Tuning& T) {
        if (!F.move || static_cast<std::size_t>(ci) >= F.move->claim.size()) return;
        MoveClaim& q = F.move->claim[static_cast<std::size_t>(ci)]; q.nTop = static_cast<std::uint8_t>(MFO::Iaus::TopRanked(d, g.actions, q.top, MFO::Iaus::MoveAsk{ g.moveOnly, g.moveOnlySight, F.ctl[static_cast<std::size_t>(ci)].mind, F.ctl[static_cast<std::size_t>(ci)].profile, T })); q.topAt = F.now;
    }

    inline bool MoveWalk(Fight& F, Body& x) {
        if (g_road == Road::Strict) return StrictNoAttack(F, x);   // his feet are StrictFeet's. He fights on the way, except in a disengage
        MoveClaim& q = F.move->claim[static_cast<std::size_t>(x.ctl)]; if (!q.on || MoveBodyTaken(F, x)) return false;
        const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)];
        const float d = std::hypot(q.p.x - x.c.x, q.p.y - x.c.y), dz = std::fabs(q.p.z - x.c.z); const bool near = dz < g_envT.storeyU;
        if (c.leash > 0.0f && std::hypot(q.p.x - F.b[0].c.x, q.p.y - F.b[0].c.y) > c.leash + kLeashStandU) return false;   // a leash wins over a point (the pass is told at the next window)
        if (near && d <= g_moveT.insideU) { if (!q.arrived) { q.arrived = true; MoveTrustLeg(F, x, TR::End::Arrived, true, q.p); q.len = 0.0f; if (x.ctl == 0) ++F.t.spotArrived; if (F.trace) std::fprintf(F.trace, "%7.2f  SPOT REACHED %s\n", F.now, x.c.name.c_str()); } }
        else if (q.arrived && (!near || d > MoveRadius())) q.arrived = false;   // pushed out of the radius: he walks back in
        auto step = [&](float tx, float ty, float speed, const EnvPoint* pt) { x.ranLast = speed >= 0.9f * x.spec->run; EnvMove(F, x, tx, ty, x.sneaking ? speed * g_moveT.sneakX : speed, false, nullptr, pt); };
        if (q.arrived) {
            if (g_road == Road::Point && g_moveT.driftU > 0.0f && !F.Busy(x)) {   // he shifts about inside R
                if (F.now >= q.driftAt) { q.driftAt = F.now + static_cast<double>(g_moveT.driftSec); const std::uint64_t h = Fight::Mix64(F.avSeed ^ Fight::Mix64(q.key) ^ static_cast<std::uint64_t>(F.now / static_cast<double>(g_moveT.driftSec)) ^ (static_cast<std::uint64_t>(x.c.id) << 32));
                    const float a = static_cast<float>(h & 1023) * (6.2831853f / 1024.0f), r = g_moveT.driftU * static_cast<float>((h >> 10) & 1023) / 1024.0f;
                    const EQ::Walk w = EQ::WalkLine(EnvViewOf(F), x.nav.tri, { x.c.x, x.c.y, x.c.z }, { q.p.x + r * std::cos(a), q.p.y + r * std::sin(a), x.c.z }, WalkerOf(x)); q.dx = w.end.x; q.dy = w.end.y; }
                if (std::hypot(q.dx - x.c.x, q.dy - x.c.y) > 20.0f && (q.dx != 0.0f || q.dy != 0.0f)) EnvTryStep(F, x, q.dx, q.dy, g_moveT.driftSpeedX * x.spec->run * TM::dt); }
            return false;   // he holds: the rest of his combat goes on, MoveBounds keeps him in
        }
        if (q.dropDue && F.now - q.since > 0.5) { MV::Member* m = MoveMember(F, x.c.id); if (m) { m->marks |= MV::kMarkPathBlocked; m->commit = MV::Commit{}; } MoveTrustLeg(F, x, TR::End::Refused, true, q.p); if (x.ctl == 0) ++F.t.spotDropped; MoveRelease(F, x.ctl, "the engine dropped it"); return false; }
        if (g_moveT.fightsOnTheWay && (c.drawn == PostureKind::Melee || c.drawn == PostureKind::Silver))   // "the actor fights the whole way": a foe in reach is swung at
            for (const Body& o : F.b) if (o.role == Role::Foe && Fight::Up(o) && Fight::Dist(x, o) <= F.Reach(x) * 0.9f && EnvMeleeClear(F, x, o)) return false;
        EnvPoint pt; pt.p = q.p; pt.tri = q.tri;
        if (F.now - q.walkAt > 2.5 * static_cast<double>(TM::dt) && q.walked > 0.0f) q.len = 0.0f;   // something else moved him in between: the two lengths no longer measure one walk
        const EQ::Vec3 from{ x.c.x, x.c.y, x.c.z }; q.walkAt = F.now; q.walks = true;
        step(q.p.x, q.p.y, g_moveT.speedX * x.spec->run, &pt); q.walks = false; Fight::Face(x, q.p.x, q.p.y);
        q.walked += MV::Dist(from, { x.c.x, x.c.y, x.c.z });
        for (std::size_t i = 0; i < q.via.size(); ++i) if (!(q.passed & (1u << i)) && MV::Dist(q.via[i], { x.c.x, x.c.y, x.c.z }) < 100.0f) q.passed |= 1u << i;
        if (q.on && q.len > 0.0f && TR::Seen(TR::Sign::LengthFar, q.walked / q.len)) { if (q.walked - q.len >= F.move->pic.trust.rules.lengthFloorU) { MoveTrustLeg(F, x, TR::End::Walking, true, q.p); q.len = 0.0f; } }   // once a claim
        return q.on;
    }

    inline bool MoveHeld(const Fight& F, const Body& x) { const MoveClaim& q = F.move->claim[static_cast<std::size_t>(x.ctl)]; if (g_road == Road::Strict) return StrictLive(F, x); return q.on && q.arrived && !MoveBodyTaken(F, x); }

    inline bool MoveBounds(const Fight& F, const Body& x, float tx, float ty, bool away) {
        if (g_road == Road::Strict) return StrictLive(F, x);   // under an order no movement is his own
        const MoveClaim& q = F.move->claim[static_cast<std::size_t>(x.ctl)]; if (!q.on || MoveBodyTaken(F, x)) return false;
        const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; if (c.leash > 0.0f && std::hypot(q.p.x - F.b[0].c.x, q.p.y - F.b[0].c.y) > c.leash + kLeashStandU) return false;
        return away || g_road == Road::Pinned || std::hypot(tx - q.p.x, ty - q.p.y) > MoveRadius();
    }

    // A leg toward the claimed point ended (EnvLegEnd). BLOCKED and UNREACHABLE are the engine dropping the path: the claim is over. STUCK only marks him.
    inline void MoveLegEnd(Fight& F, Body& x, LegEnd how, const EQ::Vec3& toward) {
        MoveClaim& q = F.move->claim[static_cast<std::size_t>(x.ctl)];
        if (g_road == Road::Strict && !q.on && q.walks && how != LegEnd::Stuck) { if (!g_moveFault->noBlock) StrictBlock(F, x, q); return; }   // an order on his own place
        if (g_road == Road::Strict && q.on && q.walks && how != LegEnd::Stuck && !(g_residue.blockTold > 0.0f)) { if (!g_moveFault->noBlock) StrictBlock(F, x, q); return; }   // nobody is told
        if (!q.on || (q.arrived && g_road != Road::Strict) || x.nav.goalBody != 0 || (!q.walks && !g_moveFault->anyLeg)) return;   // walks: the leg was HIS claim's. Give ground, a run, a regroup have no goal body either, and a claim that waits under them keeps its place (S2 review F6)
        MV::Member* m = MoveMember(F, x.c.id); if (m) m->marks |= MV::kMarkPathBlocked;
        MoveTrustLeg(F, x, how == LegEnd::Stuck ? TR::End::Stuck : how == LegEnd::Blocked ? TR::End::Blocked : TR::End::Refused, MoveLaneClear(F, x, toward), toward);
        if (x.ctl == 0) ++F.t.spotBlocked;
        if (how == LegEnd::Stuck) return;
        if (g_road == Road::Strict) { if (!g_moveFault->noBlock && StrictBlock(F, x, q)) MoveRelease(F, x.ctl, "Unreachable"); return; }   // three blocks end an order, one does not
        if (m) m->commit = MV::Commit{};
        MoveRelease(F, x.ctl, how == LegEnd::Blocked ? "the path ended BLOCKED" : "no path");
    }

    // --spots-replace: a family whose spot kind has taken it over files no drive of its own. 0 = not replaced, 1 = his claim carries it, -1 = refused this lap.
    inline int MoveReplaces(const Fight& F, int ci, Family fam) {
        if (!g_spotsReplace || !F.move || !MV::IsMove(fam)) return 0;
        std::uint32_t kinds = 0; for (const MV::SpotKindRow& r : MV::kSpotKinds) if ((g_spotsReplace & MV::KindBit(r.kind)) && (r.families & MV::F(fam))) kinds |= MV::KindBit(r.kind);
        if (!kinds) return 0;
        const MoveClaim& q = F.move->claim[static_cast<std::size_t>(ci)];
        return q.on && (q.kinds & kinds) && q.act.family == fam ? 1 : -1;
    }

}

#include "iaus_sim/StrictWorld.h"
