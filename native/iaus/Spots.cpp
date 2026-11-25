// iaus/Spots.cpp -- spot generators, the spot considerations' combination, and the mesh helpers of the movement pass (S1). PURE.
#include "iaus/Spots.h"
#include "iaus/Iaus_internal.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <tuple>

namespace MFO::Iaus::Move {

    float Dist(const Env::Vec3& a, const Env::Vec3& b) { const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z; return std::sqrt(x * x + y * y + z * z); }

    namespace {
        float Dist2D(const Env::Vec3& a, const Env::Vec3& b) { const float x = a.x - b.x, y = a.y - b.y; return std::sqrt(x * x + y * y); }
        Env::Vec3 Mid(const Env::Vec3& a, const Env::Vec3& b) { return { 0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (a.z + b.z) }; }
        Env::Vec3 Eye(Env::Vec3 p, const Tuning& t) { p.z += t.spotEyeU; return p; }
        const Foe* ThreatOf(const GenIn& g, std::size_t i) {
            const auto k = g.m.threats[i];
            return k >= 0 && static_cast<std::size_t>(k) < g.pic.foes.size() ? &g.pic.foes[static_cast<std::size_t>(k)] : nullptr;
        }
        // The distance the action works at: the range veto's side of the band, with a margin so a hold's drift does not leave it.
        bool InBand(const GenIn& g, const Cand& c) { return g.act.reach > 0.0f && Dist(c.p, g.target) <= 0.9f * g.act.reach; }
        bool Ranged(const GenIn& g) { return !g.act.melee; }
        bool OpenTo(const Env::View& v, const Env::Tri& n, const Env::Walker& w) {
            if (w.large && (n.flags & Env::kTriNoLarge)) return false;
            if (n.door < 0 || static_cast<std::size_t>(n.door) >= v.doors.size()) return true;
            const auto s = v.doors[static_cast<std::size_t>(n.door)].state;
            return s == Env::DoorState::Open || (w.opensDoors && (s == Env::DoorState::Closed || w.unlocks));
        }
        bool Walks(const Env::View& v, Env::Edge e) { return e == Env::Edge::Walk || (e == Env::Edge::Portal && v.portalsOpen); }
    }

    float Work::Flood::At(std::int32_t tri) const {
        const auto it = std::lower_bound(d.begin(), d.end(), tri, [](const auto& a, std::int32_t b) { return a.first < b; });
        return it != d.end() && it->first == tri ? it->second : -1.0f;
    }

    std::uint32_t Work::Offered(std::int32_t spot, std::uint32_t member, std::uint8_t action) const {
        const auto it = std::lower_bound(offers.begin(), offers.end(), std::tie(spot, member, action), [](const Offer& o, const auto& k) { return std::tie(o.spot, o.member, o.action) < k; });
        return it != offers.end() && it->spot == spot && it->member == member && it->action == action ? it->kinds : 0u;
    }

    // Dijkstra over triangle centres, nearest first, ties by triangle. An estimate of the walk: FindPath is the authority for a spot that is paired.
    void Flood(const Env::View& v, const Env::Vec3& from, std::int32_t tri, const Env::Walker& who, float radius, int maxTris, Work::Flood& out) {
        out.d.clear();
        if (tri < 0 || static_cast<std::size_t>(tri) >= v.tris.size()) return;
        using N = std::pair<float, std::int32_t>;
        std::priority_queue<N, std::vector<N>, std::greater<N>> q;
        std::vector<float> best(v.tris.size(), kInf);   // < 0 = closed
        out.cut = false; out.bound = radius;
        q.push({ 0.0f, tri }); best[static_cast<std::size_t>(tri)] = 0.0f;
        while (!q.empty()) {
            const auto [d, at] = q.top(); q.pop();
            if (best[static_cast<std::size_t>(at)] < 0.0f) continue;
            if (static_cast<int>(out.d.size()) >= maxTris) { out.cut = true; out.bound = d; break; }
            best[static_cast<std::size_t>(at)] = -1.0f;
            out.d.push_back({ at, d });
            const Env::Tri& T = v.tris[static_cast<std::size_t>(at)];
            const Env::Vec3 here = at == tri ? from : Env::Centroid(v, at);
            for (int k = 0; k < 3; ++k) {
                const std::int32_t nb = T.nb[k];
                if (nb < 0 || static_cast<std::size_t>(nb) >= v.tris.size() || !Walks(v, T.kind[k]) || best[static_cast<std::size_t>(nb)] < 0.0f) continue;
                if (!OpenTo(v, v.tris[static_cast<std::size_t>(nb)], who)) continue;
                const Env::Vec3 e = Mid(v.verts[T.v[k]], v.verts[T.v[(k + 1) % 3]]);
                const float nd = d + Dist(here, e) + Dist(e, Env::Centroid(v, nb));
                if (!(nd <= radius)) continue;
                if (best[static_cast<std::size_t>(nb)] <= nd) continue;
                best[static_cast<std::size_t>(nb)] = nd; q.push({ nd, nb });
            }
        }
        std::sort(out.d.begin(), out.d.end());
    }

    void Points(const Env::View& v, std::int32_t tri, const Tuning& t, std::vector<Cand>& out) {
        const Env::Tri& T = v.tris[static_cast<std::size_t>(tri)];
        if (T.flags & Env::kTriWater) return;   // nobody is sent to stand in water
        const Env::Vec3 c = Env::Centroid(v, tri);
        out.push_back({ KeyOfPoint(tri, 0), c, tri, 0.0f });
        const Env::Vec3 &a = v.verts[T.v[0]], &b = v.verts[T.v[1]], &d = v.verts[T.v[2]];
        const float area = 0.5f * std::fabs((b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x));
        if (!(area > t.spotSplitArea)) return;
        for (int k = 0; k < 3; ++k) out.push_back({ KeyOfPoint(tri, k + 1), Mid(c, v.verts[T.v[k]]), tri, 0.0f });
    }

    void WallVerts(const Env::View& v, std::vector<std::uint8_t>& out) {
        out.assign(v.verts.size(), 0);
        for (const Env::Tri& T : v.tris)
            for (int k = 0; k < 3; ++k)
                if (T.kind[k] == Env::Edge::Wall) { out[T.v[k]] = 1; out[T.v[(k + 1) % 3]] = 1; }
    }

    std::uint64_t DoorwayOf(const Env::View& v, const std::vector<std::uint8_t>& wall, std::int32_t tri, int k, const Tuning& t) {
        const Env::Tri& T = v.tris[static_cast<std::size_t>(tri)];
        const std::int32_t nb = T.nb[k];
        if (nb < 0 || !Walks(v, T.kind[k])) return 0;
        const std::uint32_t a = T.v[k], b = T.v[(k + 1) % 3];
        if (a >= wall.size() || b >= wall.size() || !wall[a] || !wall[b] || !(Dist2D(v.verts[a], v.verts[b]) < t.spotPortalU)) return 0;
        const auto lo = static_cast<std::uint64_t>((std::min)(tri, nb)) + 1, hi = static_cast<std::uint64_t>((std::max)(tri, nb)) + 1;
        return (lo << 32) | hi;
    }

    std::array<std::uint64_t, 2> PortalsAt(const Env::View& v, const std::vector<std::uint8_t>& wall, std::int32_t tri, const Env::Vec3& p, const Tuning& t) {
        std::array<std::uint64_t, 2> out{}; std::size_t n = 0;
        if (tri < 0 || static_cast<std::size_t>(tri) >= v.tris.size()) return out;
        const Env::Tri& T = v.tris[static_cast<std::size_t>(tri)];
        if (T.door >= 0 && static_cast<std::size_t>(T.door) < v.doors.size()) out[n++] = kStayBit | (v.doors[static_cast<std::size_t>(T.door)].id ? v.doors[static_cast<std::size_t>(T.door)].id : static_cast<std::uint32_t>(T.door) + 1u);
        for (int k = 0; k < 3 && n < out.size(); ++k) {
            const std::uint64_t id = DoorwayOf(v, wall, tri, k, t);
            if (!id) continue;
            const Env::Vec3 &a = v.verts[T.v[k]], &b = v.verts[T.v[(k + 1) % 3]];
            const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
            float s = l2 > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.0f;
            s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
            if (Dist2D(p, { a.x + dx * s, a.y + dy * s, 0.0f }) < t.spotPortalStandU) out[n++] = id;
        }
        return out;
    }

    // How many of the eight compass octants a walk out of here leads into (it must get spotExitU away), not counting one a near foe stands in.
    std::uint8_t ExitsAt(const Env::View& v, std::int32_t tri, const Env::Vec3& p, const std::vector<Foe>& foes, const Env::Walker& who, const Tuning& t) {
        if (tri < 0) return 0;
        Work::Flood f;
        Flood(v, p, tri, who, 2.0f * t.spotExitU, 96, f);
        const auto octant = [](float x, float y) { return (x >= 0.0f ? 1u : 0u) | (y >= 0.0f ? 2u : 0u) | (std::fabs(x) >= std::fabs(y) ? 4u : 0u); };
        std::uint32_t open = 0, shut = 0;
        for (const auto& [at, d] : f.d) if (d >= t.spotExitU) { const Env::Vec3 c = Env::Centroid(v, at); open |= 1u << octant(c.x - p.x, c.y - p.y); }
        // a mesh of small triangles fills the cap inside the radius: its outermost ring is the frontier
        if (!open && f.d.size() >= 96) {
            float far = 0.0f; for (const auto& e : f.d) far = (std::max)(far, e.second);
            for (const auto& [at, d] : f.d) if (d >= 0.8f * far) { const Env::Vec3 c = Env::Centroid(v, at); open |= 1u << octant(c.x - p.x, c.y - p.y); }
        }
        for (const Foe& e : foes) if (Dist(e.p, p) < 1.5f * t.spotExitU) shut |= 1u << octant(e.p.x - p.x, e.p.y - p.y);
        std::uint8_t n = 0;
        for (std::uint32_t m = open & ~shut; m; m &= m - 1) ++n;
        return n;
    }

    bool GeomSees(const Env::View& v, const Env::Vec3& a, const Env::Vec3& b, const Tuning& t, Env::Scratch& s) {
        return Env::CastRay(v, Eye(a, t), Eye(b, t), s, Env::kSightTuning, 48.0f).clear;
    }

    // ── generators. Cheap: distances and facts already there. The geometry rays are in the checks ──────────────────────
    bool GenFiring(const GenIn& g, const Cand& c, float& r) {
        if (!g.hasTarget || g.targetIsAlly || !Ranged(g) || !InBand(g, c)) return false;
        r = c.walk; return true;
    }
    bool CheckSight(const GenIn& g, const Cand& c, Env::Scratch& s) { return GeomSees(g.v, g.target, c.p, g.t, s); }

    bool GenCover(const GenIn& g, const Cand& c, float& r) {
        if (!ThreatOf(g, 0)) return false;
        if ((g.act.Kind() == Family::CastHeal || g.act.Kind() == Family::CastBuff) && g.hasTarget && g.act.reach > 0.0f && !InBand(g, c)) return false;
        r = c.walk; return true;
    }
    bool CheckCover(const GenIn& g, const Cand& c, Env::Scratch& s) {
        for (std::size_t i = 0; i < g.m.threats.size(); ++i)
            if (const Foe* f = ThreatOf(g, i); f && GeomSees(g.v, f->p, c.p, g.t, s)) return false;
        return true;
    }

    bool GenHighGround(const GenIn& g, const Cand& c, float& r) {
        if (!g.foe || !Ranged(g) || !InBand(g, c) || !(c.p.z - g.target.z > g.t.spotHighU)) return false;
        r = c.walk; return true;
    }

    bool GenChokepoint(const GenIn& g, const Cand& c, float& r) {
        if (!g.foe || !g.act.melee || PortalsAt(g.v, g.pic.wallVert, c.tri, c.p, g.t)[0] == 0) return false;
        if (!(Dist(c.p, g.target) < Dist(g.m.p, g.target))) return false;   // between him and the foe
        r = c.walk; return true;
    }

    bool GenFlank(const GenIn& g, const Cand& c, float& r) {
        if (!g.foe || (g.foe->faceX == 0.0f && g.foe->faceY == 0.0f)) return false;
        const auto front = [&](const Env::Vec3& p) {   // the cosine to the foe's facing, times the distance's square root free form
            const float d = Dist2D(p, g.target);
            return d > 1.0f ? ((p.x - g.target.x) * g.foe->faceX + (p.y - g.target.y) * g.foe->faceY) / d : 1.0f;
        };
        const float d = Dist2D(c.p, g.target);
        if (d > 300.0f || d < 60.0f || front(c.p) > -0.3f) return false;
        for (const float s : { 0.25f, 0.5f, 0.75f }) {   // the straight way there does not cross the front arc
            const Env::Vec3 p{ g.m.p.x + (c.p.x - g.m.p.x) * s, g.m.p.y + (c.p.y - g.m.p.y) * s, c.p.z };
            if (front(p) > 0.7f) return false;
        }
        r = c.walk; return true;
    }

    bool GenHealReach(const GenIn& g, const Cand& c, float& r) {
        if (!g.hasTarget || !g.targetIsAlly || !InBand(g, c)) return false;
        r = c.walk; return true;
    }

    bool GenPeel(const GenIn& g, const Cand& c, float& r) {
        if (!g.foe || g.foe->target == 0 || g.foe->target == g.m.id) return false;
        for (const Member& a : g.pic.party)
            if (a.id == g.foe->target) {
                const float off = Dist(c.p, Mid(a.p, g.foe->p));
                if (off > g.t.spotPeelU) return false;
                r = off + 0.25f * c.walk; return true;
            }
        return false;
    }

    bool GenKite(const GenIn& g, const Cand& c, float& r) {
        const Foe* f = ThreatOf(g, 0);
        if (!f || (g.act.melee && !IsMove(g.act.family)) || !(c.walk > 100.0f)) return false;
        const Work::Flood* fl = nullptr;
        for (const auto& e : g.pic.work.foeFlood) if (e.who == f->id) { fl = &e; break; }
        if (!fl) return false;
        const float his = fl->At(c.tri);
        const float lead = (his >= 0.0f ? his : fl->bound) - c.walk;   // not reached by the foe's flood: at least its bound
        if (!(lead > 150.0f)) return false;
        if (!IsMove(g.act.family) && g.hasTarget && !InBand(g, c)) return false;
        r = c.walk - lead; return true;
    }

    bool GenRegroup(const GenIn& g, const Cand& c, float& r) {
        if (!(g.m.leash > 0.0f)) return false;
        const float d = Dist(c.p, g.m.anchor);
        if (d > 0.5f * g.m.leash || d < 80.0f || !(d < Dist(g.m.p, g.m.anchor))) return false;
        r = c.walk; return true;
    }

    bool GenRetreatExit(const GenIn& g, const Cand& c, float& r) {
        const Foe* f = ThreatOf(g, 0);
        if (!f) return false;
        float near = kInf;
        for (const Foe& e : g.pic.foes) near = (std::min)(near, Dist(e.p, c.p));
        if (!(Dist(f->p, c.p) > Dist(f->p, g.m.p) + 200.0f)) return false;   // away from what is on him
        r = c.walk - near; return true;                                     // the exits are counted on the pooled spot: a dead end is dropped there
    }

    bool GenReachUnreach(const GenIn& g, const Cand& c, float& r) {
        if (!g.foe || !g.targetUnreachable || !InBand(g, c)) return false;
        r = c.walk; return true;
    }

    bool KeptHolds(SpotKind k, const GenIn& g, const Cand& c) {
        if (k != SpotKind::RetreatExit && k != SpotKind::Kite) return true;
        const Foe* f = ThreatOf(g, 0);
        return !f || Dist(f->p, c.p) > Dist(f->p, g.m.p) - g.t.spotArriveU;   // never toward what is on him now
    }

    bool IsMove(Family f) {
        return f == Family::Retreat || f == Family::Regroup || f == Family::GiveGround || f == Family::Disengage || f == Family::BackOff || f == Family::Step;
    }

    std::uint32_t AllowedKinds(const RoadCaps& c, const Tuning& t) {
        if (!c.point) return KindBit(SpotKind::Stay);
        std::uint32_t have = 0;
        if (c.pin && t.spotPinKinds >= 1.0f) have |= kNeedPin;
        if (c.facing) have |= kNeedFacing;
        if (c.gait) have |= kNeedGait;
        std::uint32_t m = 0;
        for (const SpotKindRow& r : kSpotKinds) if ((r.needs & ~have) == 0) m |= KindBit(r.kind);
        return m;
    }

    std::uint32_t SpotKindsFor(const ClassGate& g) {
        if (!g.resolved) return 0xFFFFFFFFu;   // no class known: nothing is class-gated
        std::uint32_t m = KindBit(SpotKind::Stay) | KindBit(SpotKind::HealReach) | KindBit(SpotKind::Regroup) | KindBit(SpotKind::LootRoute) | KindBit(SpotKind::ReachUnreach);
        const bool ranged = g.casterLine || g.weaponBow || g.weaponCrossbow || g.weaponStaff || g.spellsOnClass;
        const bool melee = g.weaponMelee && !g.casterLine;
        if (ranged) m |= KindBit(SpotKind::Firing) | KindBit(SpotKind::HighGround);
        if (ranged && !g.taunt) m |= KindBit(SpotKind::Kite);
        if (!g.taunt) m |= KindBit(SpotKind::Cover);
        if (melee) m |= KindBit(SpotKind::Peel) | KindBit(SpotKind::Chokepoint);
        if (melee && !g.taunt) m |= KindBit(SpotKind::Flank);
        if (g.disengage || g.backOffPressed || g.casterLine || g.sneakClass) m |= KindBit(SpotKind::RetreatExit);
        return m;
    }

    // ── scoring: the same combination as Score.cpp (compensated product of shapes, boosts outside), over the spot set ──────────────
    namespace {
        constexpr detail::Use kSpotSet[] = {
            { Cons::SpotPath, 1 }, { Cons::SpotSight, 1 }, { Cons::SpotExposure, 1 }, { Cons::SpotFireLine, 1 }, { Cons::SpotRange, 1 }, { Cons::SpotPathExposed, 1 },
            { Cons::SpotExits, 1 }, { Cons::SpotAllyDist, 1 }, { Cons::SpotHostileArea, 1 }, { Cons::SpotLeash, 1 }, { Cons::SpotClimb, 1 }, { Cons::SpotDrop, 1 },
            { Cons::SpotDetected, 1 }, { Cons::SpotLight, 1 }, { Cons::SpotFoeReach, 1 }, { Cons::SpotLane, 1 }, { Cons::SpotPortalBusy, 1 }, { Cons::SpotServes, 1 },
            { Cons::BoostSpotCover, 1 },
        };
        detail::In Num(float v) { return v < 0.0f ? detail::In::Off() : detail::In(v); }   // a NaN applies, and is refused below
        detail::In Three(Iaus::Tri v) { return v == Iaus::Tri::Unknown ? detail::In::Off() : detail::In(v == Iaus::Tri::Yes ? 1.0f : 0.0f); }
        detail::In SpotInput(Cons c, const SpotFacts& f) {
            switch (c) {
            case Cons::SpotPath:        return Num(f.pathSec);
            case Cons::SpotSight:       return Three(f.sees);
            case Cons::SpotExposure:    return Num(f.exposure);
            case Cons::SpotFireLine:    return Three(f.fireLineClear);
            case Cons::SpotRange:       return Num(f.rangeX);
            case Cons::SpotPathExposed: return Num(f.pathExposedSec);
            case Cons::SpotExits:       return Num(f.exits);
            case Cons::SpotAllyDist:    return Num(f.allyDist);
            case Cons::SpotHostileArea: return Three(f.inHostileArea);
            case Cons::SpotLeash:       return Num(f.leashX);
            case Cons::SpotClimb:       return Num(f.climb);
            case Cons::SpotDrop:        return Num(f.drop);
            case Cons::SpotDetected:    return Num(f.detected);
            case Cons::SpotLight:       return Num(f.light);
            case Cons::SpotFoeReach:    return Three(f.foeReachable);
            case Cons::SpotLane:        return Three(f.laneCrossed);
            case Cons::SpotPortalBusy:  return Three(f.portalBusy);
            case Cons::SpotServes:      return Three(f.serves);
            case Cons::BoostSpotCover:  return detail::In(f.coversAlly ? 1.0f : 0.0f);
            default:                    return detail::In::Off();
            }
        }
    }

    float ScoreSpot(const SpotFacts& f, Family fam, const Profile& p, const Tuning& t, std::vector<Term>* terms) {
        std::size_t n = 0;
        for (const auto& u : kSpotSet) {
            const detail::In in = SpotInput(u.cons, f);
            if (!in.on) continue;
            if (!std::isfinite(in.x)) return 0.0f;   // a reader bug never scores
            if (RoleOf(u.cons) == Role::Shape) ++n;
        }
        const float mod = n > 0 ? 1.0f - 1.0f / static_cast<float>(n) : 0.0f;
        float prod = 1.0f, boost = 1.0f;
        for (const auto& u : kSpotSet) {
            const detail::In in = SpotInput(u.cons, f);
            if (!in.on) continue;
            const float raw = Eval(detail::CurveFor(fam, u.cons, p, t), in.x);
            if (RoleOf(u.cons) == Role::Boost) { boost *= raw; if (terms) terms->push_back({ u.cons, in.x, raw, 1.0f, raw, raw }); continue; }
            if (!(raw > 0.0f)) { if (terms) terms->push_back({ u.cons, in.x, raw, 0.0f, 0.0f, 0.0f }); return 0.0f; }
            const float w = detail::WeightFor(fam, u, p);
            const float s = w == 1.0f ? (std::min)(raw, 1.0f) : std::pow((std::min)(raw, 1.0f), w);   // a searched weight is the one libm call: Quant absorbs its last bit
            const float comp = s + (1.0f - s) * mod * s;
            prod *= comp;
            if (terms) terms->push_back({ u.cons, in.x, raw, w, s, comp });
        }
        const float out = prod * boost;
        return std::isfinite(out) ? out : 0.0f;
    }
}
