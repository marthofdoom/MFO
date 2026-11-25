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
        return !Fight::Up(x) || c.runPhase != 0 || F.now < x.fleeUntil || F.now < x.retreatUntil || F.now < x.holdUntil || F.now < c.moveUntil || x.airborne || x.perch;
    }
    [[nodiscard]] inline MV::Member* MoveMember(Fight& F, std::uint32_t id) { for (MV::Member& m : F.move->pic.party) if (m.id == id) return &m; return nullptr; }
    inline float MoveRadius() { return g_road == Road::Pinned ? g_moveT.pinRadiusU : g_moveT.radiusU; }

    inline void MoveRelease(Fight& F, int ci, const char* why) {
        MoveClaim& q = F.move->claim[static_cast<std::size_t>(ci)]; if (!q.on) return;
        q.on = q.arrived = false; if (ci == 0) ++F.t.spotReleased;
        Body& x = F.b[static_cast<std::size_t>(F.ctl[static_cast<std::size_t>(ci)].body)]; x.nav.n = x.nav.i = 0;
        if (F.trace) std::fprintf(F.trace, "%7.2f  SPOT RELEASED %s: %s\n", F.now, x.c.name.c_str(), why);
    }

    inline std::string MoveKindNames(std::uint32_t mask) { std::string o; for (const MV::SpotKindRow& r : MV::kSpotKinds) if (mask & MV::KindBit(r.kind)) o += std::string(o.empty() ? "" : "+") + r.name; return o; }

    // The picture's inputs, from the fight as it stands. A member keeps what the pass remembers of him (commit, marks).
    inline void MoveFill(Fight& F) {
        MoveState& S = *F.move.s; MV::PartyPicture& P = S.pic;
        P.t = F.now; P.caps = MV::RoadCaps{}; P.caps.pin = P.caps.facing = g_road == Road::Pinned;
        std::vector<MV::Member> was; was.swap(P.party); P.foes.clear(); P.areas.clear();
        for (const Body& x : F.b) {
            if (!Fight::Up(x)) continue;
            if (!x.Party()) {
                MV::Foe f; f.id = x.c.id; f.p = { x.c.x, x.c.y, x.c.z }; const Body* tg = F.Find(x.c.targetId); f.target = tg && tg->Party() && Fight::Up(*tg) ? tg->c.id : 0;
                f.faceX = x.hx; f.faceY = x.hy; P.foes.push_back(f); continue; }
            MV::Member m; for (MV::Member& o : was) if (o.id == x.c.id) { m = o; break; }
            m.id = x.c.id; m.p = { x.c.x, x.c.y, x.c.z }; m.who = WalkerOf(x); m.speed = x.spec->run * (x.sneaking ? 0.55f : 1.0f); m.nTop = 0; m.decides = false;
            m.anchor = { F.b[0].c.x, F.b[0].c.y, F.b[0].c.z }; m.leash = 0.0f;
            if (x.ctl >= 0) { const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; MoveClaim& q = S.claim[static_cast<std::size_t>(x.ctl)];
                m.leash = c.leash; m.classId = static_cast<std::uint8_t>(x.ctl); m.kinds = MV::SpotKindsFor(c.gate) & ~g_moveFault->kinds; m.decides = !MoveBodyTaken(F, x);
                m.top = q.top; m.nTop = q.nTop; m.ageSec = static_cast<float>(F.now - q.topAt);
                // what the holder finds: he was hit hard, and a place the leash no longer lets him stand on (a leash wins over a point, kLeashStandU past it he still stands)
                if (x.burstMax > q.burstSeen + 1.0f && x.burstMax >= kOneHitLine * x.c.hpMax) m.marks |= MV::kMarkHitHard;
                q.burstSeen = x.burstMax;
                if (m.commit.on && m.leash > 0.0f && MV::Dist(m.commit.p, m.anchor) > m.leash + kLeashStandU) m.marks |= MV::kMarkOrder;
                if (!m.decides && m.commit.on) { m.commit = MV::Commit{}; }   // another mover has him: the place is given up, he asks again when he is free
            } else m.commit = MV::Commit{};
            P.party.push_back(m);
        }
        for (const Area& a : F.areas) { const Body* o = F.Find(a.src); if (a.hostile && o && !o->Party()) P.areas.push_back({ { a.x, a.y, a.z }, a.r }); }
        for (const Hazard& h : F.hazards) if (!h.party && !h.pcOnly) P.areas.push_back({ { h.x, h.y, h.z }, h.r });   // 16.3: a hazard is a spot input, as designed
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

    inline void MoveClose(Fight& F) {
        MoveState& S = *F.move.s; const auto t0 = std::chrono::steady_clock::now(); tl_moveFight = &F;
        std::vector<MV::Assignment> as; MP::Phase3(S.pic, &MoveProfile, *ActiveTuning(), as); MP::Adopt(S.pic, as);
        g_moveNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        S.last = as;
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
            const float bs = q.burstSeen; const auto top = q.top; const auto nTop = q.nTop; const double topAt = q.topAt; q = MoveClaim{}; q.burstSeen = bs; q.top = top; q.nTop = nTop; q.topAt = topAt;
            q.on = true; q.key = m->commit.key; q.p = m->commit.p; q.tri = m->commit.tri; q.kinds = m->commit.kinds; q.act = m->commit.act; q.since = F.now;
            x->nav.n = x->nav.i = 0;   // a new claim is a new path
            if (g_moveT.dropShare > 0.0f) q.dropDue = static_cast<double>(Fight::Mix64(F.avSeed ^ Fight::Mix64(q.key ^ (static_cast<std::uint64_t>(a.who) << 40)) ^ static_cast<std::uint64_t>(S.pic.window)) >> 11) * (1.0 / 9007199254740992.0) < static_cast<double>(g_moveT.dropShare);
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
        }
        if (F.ctl.empty()) return;
        const Body& s0 = F.b[static_cast<std::size_t>(F.ctl[0].body)]; if (!Fight::Up(s0)) return;
        if (F.move && F.move->claim[0].on) { F.t.spotClaimSec += TM::dt; if (!F.move->claim[0].arrived) F.t.spotWalkSec += TM::dt; }
        if (g_spotMeter && std::floor(F.now * 2.0) != std::floor((F.now - static_cast<double>(TM::dt)) * 2.0)) {   // each half second: who that targets him sees him
            int n = 0, seen = 0; for (const Body& o : F.b) if (!o.Party() && Fight::Up(o) && o.c.targetId == s0.c.id) { ++n; seen += EnvSees(F, o, s0); }
            if (n) { F.t.threatSec += 0.5; if (!seen) F.t.coverSec += 0.5; F.t.exposedSec += 0.5 * double(seen) / double(n); } }
    }

    // His own lap's ranking, for the pass (the held action first). Called by the policy after Decide.
    inline void MoveNoteRanked(Fight& F, int ci, const Decision& d, std::span<const Candidate> acts) {
        if (!F.move || static_cast<std::size_t>(ci) >= F.move->claim.size()) return;
        MoveClaim& q = F.move->claim[static_cast<std::size_t>(ci)]; q.nTop = static_cast<std::uint8_t>(MFO::Iaus::TopRanked(d, acts, q.top)); q.topAt = F.now;
    }

    inline bool MoveWalk(Fight& F, Body& x) {
        MoveClaim& q = F.move->claim[static_cast<std::size_t>(x.ctl)]; if (!q.on || MoveBodyTaken(F, x)) return false;
        const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)];
        const float d = std::hypot(q.p.x - x.c.x, q.p.y - x.c.y), dz = std::fabs(q.p.z - x.c.z); const bool near = dz < g_envT.storeyU;
        if (c.leash > 0.0f && std::hypot(q.p.x - F.b[0].c.x, q.p.y - F.b[0].c.y) > c.leash + kLeashStandU) return false;   // a leash wins over a point (the pass is told at the next window)
        if (near && d <= g_moveT.insideU) { if (!q.arrived) { q.arrived = true; if (x.ctl == 0) ++F.t.spotArrived; if (F.trace) std::fprintf(F.trace, "%7.2f  SPOT REACHED %s\n", F.now, x.c.name.c_str()); } }
        else if (q.arrived && (!near || d > MoveRadius())) q.arrived = false;   // pushed out of the radius: he walks back in
        auto step = [&](float tx, float ty, float speed, const EnvPoint* pt) { x.ranLast = speed >= 0.9f * x.spec->run; EnvMove(F, x, tx, ty, x.sneaking ? speed * 0.55f : speed, false, nullptr, pt); };
        if (q.arrived) {
            if (g_road == Road::Point && g_moveT.driftU > 0.0f && !F.Busy(x)) {   // he shifts about inside R
                if (F.now >= q.driftAt) { q.driftAt = F.now + static_cast<double>(g_moveT.driftSec); const std::uint64_t h = Fight::Mix64(F.avSeed ^ Fight::Mix64(q.key) ^ static_cast<std::uint64_t>(F.now / static_cast<double>(g_moveT.driftSec)) ^ (static_cast<std::uint64_t>(x.c.id) << 32));
                    const float a = static_cast<float>(h & 1023) * (6.2831853f / 1024.0f), r = g_moveT.driftU * static_cast<float>((h >> 10) & 1023) / 1024.0f;
                    const EQ::Walk w = EQ::WalkLine(EnvViewOf(F), x.nav.tri, { x.c.x, x.c.y, x.c.z }, { q.p.x + r * std::cos(a), q.p.y + r * std::sin(a), x.c.z }, WalkerOf(x)); q.dx = w.end.x; q.dy = w.end.y; }
                if (std::hypot(q.dx - x.c.x, q.dy - x.c.y) > 20.0f && (q.dx != 0.0f || q.dy != 0.0f)) EnvTryStep(F, x, q.dx, q.dy, g_moveT.driftSpeedX * x.spec->run * TM::dt); }
            return false;   // he holds: the rest of his combat goes on, MoveBounds keeps him in
        }
        if (q.dropDue && F.now - q.since > 0.5) { MV::Member* m = MoveMember(F, x.c.id); if (m) { m->marks |= MV::kMarkPathBlocked; m->commit = MV::Commit{}; } if (x.ctl == 0) ++F.t.spotDropped; MoveRelease(F, x.ctl, "the engine dropped it"); return false; }
        if (g_moveT.fightsOnTheWay && (c.drawn == PostureKind::Melee || c.drawn == PostureKind::Silver))   // "the actor fights the whole way": a foe in reach is swung at
            for (const Body& o : F.b) if (o.role == Role::Foe && Fight::Up(o) && Fight::Dist(x, o) <= F.Reach(x) * 0.9f && EnvMeleeClear(F, x, o)) return false;
        EnvPoint pt; pt.p = q.p; pt.tri = q.tri;
        step(q.p.x, q.p.y, g_moveT.speedX * x.spec->run, &pt); Fight::Face(x, q.p.x, q.p.y);
        return q.on;
    }

    inline bool MoveHeld(const Fight& F, const Body& x) { const MoveClaim& q = F.move->claim[static_cast<std::size_t>(x.ctl)]; return q.on && q.arrived && !MoveBodyTaken(F, x); }

    inline bool MoveBounds(const Fight& F, const Body& x, float tx, float ty, bool away) {
        const MoveClaim& q = F.move->claim[static_cast<std::size_t>(x.ctl)]; if (!q.on || MoveBodyTaken(F, x)) return false;
        const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; if (c.leash > 0.0f && std::hypot(q.p.x - F.b[0].c.x, q.p.y - F.b[0].c.y) > c.leash + kLeashStandU) return false;
        return away || g_road == Road::Pinned || std::hypot(tx - q.p.x, ty - q.p.y) > MoveRadius();
    }

    // A leg toward the claimed point ended (EnvLegEnd). BLOCKED and UNREACHABLE are the engine dropping the path: the claim is over. STUCK only marks him.
    inline void MoveLegEnd(Fight& F, Body& x, LegEnd how) {
        MoveClaim& q = F.move->claim[static_cast<std::size_t>(x.ctl)]; if (!q.on || q.arrived || x.nav.goalBody != 0) return;
        MV::Member* m = MoveMember(F, x.c.id); if (m) m->marks |= MV::kMarkPathBlocked;
        if (x.ctl == 0) ++F.t.spotBlocked;
        if (how == LegEnd::Stuck) return;
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
