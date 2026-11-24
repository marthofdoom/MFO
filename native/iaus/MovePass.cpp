// iaus/MovePass.cpp -- the movement pass (S1): freeze, the sliced phase 1, the answers, the assignment. PURE (iaus/MovePass.h).
#include "iaus/MovePass.h"
#include "iaus/Estimate.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace MFO::Iaus::MovePass {

    std::int64_t Quant(float s) { return std::isfinite(s) ? static_cast<std::int64_t>(std::floor(static_cast<double>(s) * 4096.0 + 0.5)) : 0; }

    namespace {
        constexpr const MoveBudget& B = kMoveBudget;
        std::int32_t QDist(float d) { return std::isfinite(d) ? static_cast<std::int32_t>(std::floor(static_cast<double>((std::clamp)(d, -1.0e6f, 1.0e6f)) + 0.5)) : 1000000; }
        bool Finite(const Env::Vec3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
        bool SameAct(const RankedAction& a, const RankedAction& b) { return a.family == b.family && a.form == b.form && a.target == b.target; }
        bool Sighted(const RankedAction& a) { return a.sight != Sight::None && !a.melee && a.target != 0; }

        template <class V> auto* ById(V& v, FormID id) {
            const auto it = std::lower_bound(v.begin(), v.end(), id, [](const auto& m, FormID k) { return m.id < k; });
            return it != v.end() && it->id == id ? &*it : nullptr;
        }
        bool ActorAt(const PartyPicture& pic, FormID id, Env::Vec3& p, std::int32_t& tri) {
            if (const Foe* f = ById(pic.foes, id)) { p = f->p; tri = f->tri; return true; }
            if (const Member* m = ById(pic.party, id)) { p = m->p; tri = m->tri; return true; }
            return false;
        }
        Iaus::Tri Sees(const PartyPicture& pic, SpotKey spot, FormID who) {
            const auto it = pic.facts.find({ spot, who });
            return it == pic.facts.end() ? Iaus::Tri::Unknown : it->second.sees;
        }
        std::int32_t Locate(const Env::View& v, const Env::Vec3& p) {
            if (v.tris.empty() || !v.grid || !Finite(p)) return -1;
            if (const auto t = Env::TriAt(v, p); t >= 0 && std::fabs(Env::HeightAt(v, t, p.x, p.y) - p.z) < 128.0f) return t;
            Env::Vec3 on; return Env::NearestTri(v, p, 128.0f, on);
        }
        bool Scores(const Member& m) { return m.decides && !m.fixed && m.nTop > 0 && m.tri >= 0; }
        std::uint32_t KindsOf(const PartyPicture& pic, const Member& m, const Tuning& t) { return AllowedKinds(pic.caps, t) & m.kinds; }
        // The kinds of this spot that this action may use, as a mask.
        std::uint32_t Pairing(std::uint32_t spotKinds, std::uint32_t allowed, const RankedAction& a) {
            std::uint32_t out = 0;
            for (std::size_t k = 1; k < kSpotKindCount; ++k)
                if ((spotKinds & allowed & (1u << k)) && Pairs(static_cast<SpotKind>(k), a)) out |= 1u << k;
            return out;
        }
        bool PairsAny(std::uint32_t spotKinds, std::uint32_t allowed, const Member& m) {
            for (std::size_t i = 0; i < m.nTop; ++i) if (Pairing(spotKinds, allowed, m.top[i])) return true;
            return false;
        }

        // ── the marks one member's place puts on the others ──
        struct Plan { bool set = false; Env::Vec3 p; bool lane = false; Env::Vec3 to; std::array<std::uint64_t, 2> portals{}; SpotKey key = 0; };

        float SegDist(const Env::Vec3& p, const Env::Vec3& a, const Env::Vec3& b) {   // Estimate::SegmentDist's math over points (a Combatant is too heavy for this loop)
            const float bx = b.x - a.x, by = b.y - a.y, bz = b.z - a.z, px = p.x - a.x, py = p.y - a.y, pz = p.z - a.z;
            const float l2 = bx * bx + by * by + bz * bz;
            if (!(l2 > 1.0f)) return Dist(p, a);
            const float u = (px * bx + py * by + pz * bz) / l2;
            if (!(u >= 0.0f && u <= 1.0f)) return kInf;
            return Dist(p, { a.x + bx * u, a.y + by * u, a.z + bz * u });
        }
        bool Cross(const Env::Vec3& a, const Env::Vec3& b, const Env::Vec3& c, const Env::Vec3& d) {   // two lanes, seen from above
            const auto side = [](const Env::Vec3& o, const Env::Vec3& u, const Env::Vec3& w) { return (u.x - o.x) * (w.y - o.y) - (u.y - o.y) * (w.x - o.x); };
            return side(a, b, c) * side(a, b, d) < 0.0f && side(c, d, a) * side(c, d, b) < 0.0f;
        }

        // THE ONE FILLER of SpotFacts. Static: what no other member's choice changes. a_pair / a_spot null = stay here.
        SpotFacts FillStatic(const PartyPicture& pic, const Member& m, const RankedAction& a, const Spot* spot, const Work::Pair* pair) {
            SpotFacts f;
            const Env::Vec3 at = spot ? spot->p : m.p;
            const SpotKey key = spot ? spot->key : KeyOfStay(m.id);
            const float speed = m.speed > 1.0f ? m.speed : 1.0f;
            f.pathSec = pair ? pair->path.length / speed : 0.0f;
            f.climb = pair ? pair->path.climb : 0.0f;
            f.drop = pair ? pair->path.drop : 0.0f;
            Env::Vec3 tp; std::int32_t tt = -1;
            const bool hasTarget = a.target != 0 && a.target != m.id && ActorAt(pic, a.target, tp, tt);
            const bool foe = hasTarget && ById(pic.foes, a.target) != nullptr;
            if (hasTarget && Sighted(a)) {
                if (spot) f.sees = Sees(pic, key, a.target);
                else f.sees = a.sight == Sight::Visible ? Iaus::Tri::Yes : a.sight == Sight::Occluded ? Iaus::Tri::No : Iaus::Tri::Unknown;   // the action facet's own ray
            }
            if (hasTarget && a.reach > 0.0f) {
                const float d = Dist(at, tp);
                f.rangeX = a.melee ? (d <= a.reach ? 0.5f : 1.0f + (d - a.reach) / 3000.0f) : d / a.reach;   // melee closes by itself: a shade, not a band
            }
            if (foe && a.melee) if (const auto it = pic.work.reach.find({ m.id, a.target }); it != pic.work.reach.end()) f.foeReachable = it->second;
            int known = 0, seen = 0, threats = 0;
            for (const std::int32_t i : m.threats) if (i >= 0) {
                ++threats;
                const auto s = Sees(pic, key, pic.foes[static_cast<std::size_t>(i)].id);
                if (s != Iaus::Tri::Unknown) { ++known; if (s == Iaus::Tri::Yes) ++seen; }
            }
            if (known) f.exposure = static_cast<float>(seen) / static_cast<float>(known);
            if (threats) { f.pathExposedSec = pair ? pair->exposedSec : 0.0f; f.exits = static_cast<float>(spot ? spot->exits : m.exits); }
            if (!pic.areas.empty()) {
                f.inHostileArea = Iaus::Tri::No;
                for (const Area& r : pic.areas) if (Dist(at, r.p) < r.r) f.inHostileArea = Iaus::Tri::Yes;
            }
            if (m.leash > 0.0f) f.leashX = Dist(at, m.anchor) / m.leash;
            if (m.wantsDark) for (const LightAt& l : pic.light) if (l.key == key) f.light = l.level;
            if (IsMove(a.Kind())) f.serves = spot ? Iaus::Tri::Yes : Iaus::Tri::No;
            return f;
        }
        // Marks: what the places already given change. Pure over (f, plans): calling it again with more plans rewrites every field it owns.
        void FillMarks(SpotFacts& f, const PartyPicture& pic, std::size_t me, const RankedAction& a, const Spot* spot, const Work::Pair* pair, const std::vector<Plan>& plans) {
            const Member& m = pic.party[me];
            const Env::Vec3 at = spot ? spot->p : m.p;
            const SpotKey key = spot ? spot->key : KeyOfStay(m.id);
            Env::Vec3 tp; std::int32_t tt = -1;
            const bool lane = Sighted(a) && a.target != m.id && ActorAt(pic, a.target, tp, tt) && ById(pic.foes, a.target);
            f.allyDist = -1.0f; f.coversAlly = false;
            f.fireLineClear = lane ? Iaus::Tri::Yes : Iaus::Tri::Unknown;
            f.laneCrossed = f.portalBusy = Iaus::Tri::Unknown;
            for (std::size_t i = 0; i < pic.party.size(); ++i) {
                if (i == me) continue;
                const Plan& o = plans[i];
                const Env::Vec3 body = o.set ? o.p : pic.party[i].p;   // not chosen yet: where he stands
                if (lane && Finite(body) && SegDist(body, at, tp) <= Estimate::kFireLinePad) f.fireLineClear = Iaus::Tri::No;
                if (!o.set) continue;
                const float d = Dist(at, o.p);
                if (f.allyDist < 0.0f || d < f.allyDist) f.allyDist = d;
                if (o.lane) {
                    const bool crossed = SegDist(at, o.p, o.to) <= Estimate::kFireLinePad || (lane && Cross(at, tp, o.p, o.to));
                    if (crossed) f.laneCrossed = Iaus::Tri::Yes; else if (f.laneCrossed == Iaus::Tri::Unknown) f.laneCrossed = Iaus::Tri::No;
                }
                if (pair) {
                    bool busy = false;
                    for (std::size_t k = 0; k < pair->path.nPortals; ++k) for (const auto id : o.portals) if (id && id == pair->path.portals[k]) busy = true;
                    if (busy) f.portalBusy = Iaus::Tri::Yes; else if (f.portalBusy == Iaus::Tri::Unknown) f.portalBusy = Iaus::Tri::No;
                }
                for (const Foe& e : pic.foes) if (e.target == pic.party[i].id && Sees(pic, key, e.id) == Iaus::Tri::Yes) f.coversAlly = true;
            }
        }
        bool Taken(const Spot& s, std::size_t me, const std::vector<Plan>& plans) {
            for (std::size_t i = 0; i < plans.size(); ++i) {
                if (i == me || !plans[i].set) continue;
                if (plans[i].key == s.key) return true;
                for (const auto a : s.portals) for (const auto b : plans[i].portals) if (a && a == b) return true;   // one body per portal edge
            }
            return false;
        }
        float KindWeight(std::uint32_t mask, const Profile& p) {
            float w = 0.0f;
            for (std::size_t k = 0; k < kSpotKindCount; ++k) if (mask & (1u << k)) w = (std::max)(w, p.spot[k]);
            return w;
        }

        struct Triple { std::uint32_t member; std::uint8_t action; std::int32_t spot; const Work::Pair* pair; float scale; SpotFacts f; float score = 0.0f; std::int64_t q = 0; bool dead = false, incumbent = false; };

        float ScoreTriple(const PartyPicture& pic, Triple& x, const ClassProfiles& profiles, const Tuning& t) {
            const Member& m = pic.party[x.member];
            const RankedAction& a = m.top[x.action];
            const float s = a.score * x.scale * ScoreSpot(x.f, a.Kind(), profiles(m.classId), t);
            return std::isfinite(s) && s > 0.0f ? s : 0.0f;
        }
        Triple MakeTriple(const PartyPicture& pic, std::uint32_t mi, std::uint8_t ai, std::int32_t si, const Work::Pair* pair, std::uint32_t mask, const ClassProfiles& profiles, const Tuning& t) {
            const Member& m = pic.party[mi];
            const Spot* s = si >= 0 ? &pic.pool[static_cast<std::size_t>(si)] : nullptr;
            Triple x{ mi, ai, si, pair, 1.0f, FillStatic(pic, m, m.top[ai], s, pair) };
            x.incumbent = s && m.commit.on && m.commit.key == s->key;
            x.scale = KindWeight(mask, profiles(m.classId)) * (x.incumbent ? t.spotMomentum : 1.0f);
            return x;
        }
        std::int32_t TriOf(const PartyPicture& pic, const Triple& x) { return x.spot >= 0 ? pic.pool[static_cast<std::size_t>(x.spot)].tri : -1; }
        // Highest score, then FormID (the party is sorted by it), then triangle, then key, then action.
        bool Before(const PartyPicture& pic, const Triple& a, const Triple& b) {
            if (a.q != b.q) return a.q > b.q;
            if (a.member != b.member) return a.member < b.member;
            if (const auto ta = TriOf(pic, a), tb = TriOf(pic, b); ta != tb) return ta < tb;
            if (a.spot != b.spot) return a.spot < b.spot;   // the pool is sorted by key
            return a.action < b.action;
        }

        // ── phase 1, one unit at a time ──
        void PathTo(PartyPicture& pic, const Env::View& v, Env::Scratch& s, const Tuning& t, const Member& m, const PathKey& k, const Env::Vec3& to, PathFact*& out) {
            out = nullptr;
            if (const auto it = pic.paths.find(k); it != pic.paths.end() && Dist(it->second.from, m.p) <= t.spotFactMoveU && (!k.actor || Dist(it->second.at[0], to) <= t.spotFactMoveU)) { out = &it->second; return; }
            if (pic.counts.paths >= static_cast<std::uint32_t>(B.paths)) { ++pic.counts.pathsSkipped; return; }
            ++pic.counts.paths;
            Env::Path p; PathFact f; f.from = m.p;
            const bool any = Env::FindPath(v, { m.p, to, m.tri, k.toTri, m.who }, s, p);
            f.reachable = any && p.reachable; f.length = p.length; f.climb = p.climb; f.drop = p.fall;
            if (k.actor) { f.at[0] = to; f.nAt = 1; }
            else if (f.reachable) {
                const int n = (std::min)(B.pathSamples, static_cast<int>(f.at.size()));   // evenly along the walk
                Env::Vec3 prev = m.p; float done = 0.0f; int next = 1;
                for (const auto& c : p.pts) {
                    const float leg = Dist(prev, c.p);
                    while (next <= n && leg > 0.0f && p.length * static_cast<float>(next) <= (done + leg) * static_cast<float>(n + 1)) {
                        const float at = (p.length * static_cast<float>(next) / static_cast<float>(n + 1) - done) / leg;
                        f.at[f.nAt++] = { prev.x + (c.p.x - prev.x) * at, prev.y + (c.p.y - prev.y) * at, prev.z + (c.p.z - prev.z) * at };
                        ++next;
                    }
                    done += leg; prev = c.p;
                }
                if (m.tri != k.toTri)
                    for (std::size_t i = 0; i + 1 < s.corridor.size() && f.nPortals < f.portals.size(); ++i) {
                        const auto add = [&](std::uint64_t id) { if (id && f.nPortals < f.portals.size() && std::find(f.portals.begin(), f.portals.begin() + f.nPortals, id) == f.portals.begin() + f.nPortals) f.portals[f.nPortals++] = id; };
                        const std::int32_t a = s.corridor[i].tri, b = s.corridor[i + 1].tri;
                        for (int e = 0; e < 3; ++e) if (v.tris[static_cast<std::size_t>(a)].nb[e] == b) add(DoorwayOf(v, pic.wallVert, a, e, t));
                        if (const auto d = v.tris[static_cast<std::size_t>(b)].door; d >= 0 && static_cast<std::size_t>(d) < v.doors.size())
                            add(kStayBit | (v.doors[static_cast<std::size_t>(d)].id ? v.doors[static_cast<std::size_t>(d)].id : static_cast<std::uint32_t>(d) + 1u));
                    }
            }
            out = &(pic.paths[k] = f);
        }

        // One flood (a member's with the reach of his foes, then the foes'). False = none left.
        bool UnitFlood(PartyPicture& pic, const Env::View& v, Env::Scratch& s, const Tuning& t) {
            Work& w = pic.work;
            const std::size_t i = w.cursor++;
            if (i < pic.party.size()) {
                const Member& m = pic.party[i];
                if (!Scores(m)) return true;
                w.memberFlood[i].who = m.id;
                Flood(v, m.p, m.tri, m.who, t.spotRadius, B.gatherTris, w.memberFlood[i]);
                for (std::size_t k = 0; k < m.nTop; ++k) {
                    const Foe* f = ById(pic.foes, m.top[k].target);
                    if (!f || f->tri < 0 || w.reach.count({ m.id, f->id })) continue;   // a foe off the mesh: unknown, never "unreachable"
                    PathFact* p = nullptr;
                    PathTo(pic, v, s, t, m, { m.id, m.tri, f->id, f->tri, true }, f->p, p);
                    if (p) w.reach[{ m.id, f->id }] = p->reachable ? Iaus::Tri::Yes : Iaus::Tri::No;
                }
                return true;
            }
            const std::size_t j = i - pic.party.size();
            if (j >= w.foeFlood.size()) return false;
            if (const Foe* f = ById(pic.foes, w.foeFlood[j].who)) Flood(v, f->p, f->tri, Env::Walker{}, t.spotRadius, B.gatherTris, w.foeFlood[j]);
            return true;
        }

        void UnitGenerate(PartyPicture& pic, const Env::View& v, Env::Scratch& s, const Tuning& t) {
            Work& w = pic.work;
            const Work::GenUnit u = w.gen[w.cursor++];
            const Member& m = pic.party[u.member];
            const RankedAction& a = m.top[u.action];
            const SpotKindRow& row = kSpotKinds[u.kind];
            GenIn g{ pic, v, t, m, a, w.memberFlood[u.member], {} };
            std::int32_t tt = -1;
            g.hasTarget = a.target != 0 && a.target != m.id && ActorAt(pic, a.target, g.target, tt);
            g.foe = g.hasTarget ? ById(pic.foes, a.target) : nullptr;
            g.targetIsAlly = g.hasTarget && !g.foe;
            if (const auto it = w.reach.find({ m.id, a.target }); it != w.reach.end()) g.targetUnreachable = it->second == Iaus::Tri::No;
            struct R { std::int32_t q; Cand c; };
            std::vector<R> found; std::vector<Cand> pts;
            for (const auto& [tri, d] : g.own.d) {
                pts.clear(); Points(v, tri, t, pts);
                const Env::Vec3 c = Env::Centroid(v, tri);
                for (Cand& k : pts) {
                    k.walk = tri == m.tri ? Dist(m.p, k.p) : d + Dist(c, k.p);
                    float r = 0.0f;
                    if (row.gen(g, k, r) && std::isfinite(r)) found.push_back({ QDist(r), k });
                }
            }
            std::sort(found.begin(), found.end(), [](const R& x, const R& y) { return x.q != y.q ? x.q < y.q : x.c.key < y.c.key; });
            int kept = 0, tested = 0;
            for (const R& x : found) {
                if (kept >= B.perGen) break;
                if (row.check) { if (tested++ >= B.geomPerGen) break; if (!row.check(g, x.c, s)) continue; }
                Spot sp; sp.key = x.c.key; sp.p = x.c.p; sp.tri = x.c.tri; sp.kinds = KindBit(row.kind);
                w.hits.push_back({ sp, static_cast<std::int32_t>(u.member), kept * 100000 + (std::min)(99999, QDist(x.c.walk)) });   // the first of every kind before the second of any, nearest first
                ++kept;
            }
        }

        void UnitPool(PartyPicture& pic, const Env::View& v, const Tuning& t) {
            Work& w = pic.work;
            std::map<SpotKey, Work::Hit> all;
            for (const Work::Hit& h : w.hits) {
                auto [it, fresh] = all.try_emplace(h.s.key, h);
                if (!fresh) { it->second.s.kinds |= h.s.kinds; it->second.rank = (std::min)(it->second.rank, h.rank); }
            }
            pic.counts.generated = static_cast<std::uint32_t>(all.size());
            std::vector<Work::Hit> list;
            for (auto& [key, h] : all) {
                h.s.portals = PortalsAt(v, pic.wallVert, h.s.tri, h.s.p, t);
                h.s.exits = ExitsAt(v, h.s.tri, h.s.p, pic.foes, Env::Walker{}, t);
                if (h.s.exits == 0) h.s.kinds &= ~KindBit(SpotKind::RetreatExit);   // a dead end is not offered
                bool in = false;   // a committed spot is in the pool already (Freeze): it gains the kinds found this window
                for (Spot& p : pic.pool) if (p.key == key) { p.kinds |= h.s.kinds; in = true; }
                if (!in && h.s.kinds) list.push_back(h);
            }
            std::sort(list.begin(), list.end(), [](const Work::Hit& a, const Work::Hit& b) { return a.rank != b.rank ? a.rank < b.rank : a.s.key < b.s.key; });
            const std::size_t room = static_cast<std::size_t>((std::max)(0, B.spots - static_cast<int>(pic.pool.size())));
            if (list.size() > room) { pic.counts.spotsDropped += static_cast<std::uint32_t>(list.size() - room); list.resize(room); }
            for (const auto& h : list) pic.pool.push_back(h.s);
            std::sort(pic.pool.begin(), pic.pool.end(), [](const Spot& a, const Spot& b) { return a.key < b.key; });

            // who may walk where, each member's nearest first, one round at a time
            struct U { std::int32_t round; Work::PathUnit u; };
            std::vector<U> us;
            for (std::uint32_t i = 0; i < pic.party.size(); ++i) {
                const Member& m = pic.party[i];
                if (!Scores(m)) continue;
                const std::uint32_t allowed = KindsOf(pic, m, t);
                std::vector<Work::PathUnit> mine;
                for (std::size_t k = 0; k < pic.pool.size(); ++k) {
                    const Spot& sp = pic.pool[k];
                    const bool own = m.commit.on && m.commit.key == sp.key;
                    if (!PairsAny(sp.kinds, allowed, m)) continue;
                    const float fl = w.memberFlood[i].At(sp.tri);
                    if (fl < 0.0f && !own) continue;   // beyond his own gather: another member's spot
                    mine.push_back({ i, static_cast<std::int32_t>(k), 0, own ? -1 : QDist(fl + Dist(Env::Centroid(v, sp.tri), sp.p)) });
                }
                std::sort(mine.begin(), mine.end(), [](const auto& a, const auto& b) { return a.near != b.near ? a.near < b.near : a.spot < b.spot; });
                for (std::size_t k = 0; k < mine.size(); ++k) us.push_back({ static_cast<std::int32_t>(k), mine[k] });
            }
            std::sort(us.begin(), us.end(), [](const U& a, const U& b) {
                if (a.round != b.round) return a.round < b.round;
                if (a.u.near != b.u.near) return a.u.near < b.u.near;
                return a.u.member < b.u.member; });
            for (const U& u : us) w.pathUnits.push_back(u.u);
        }

        void UnitPath(PartyPicture& pic, const Env::View& v, Env::Scratch& s, const Tuning& t) {
            Work& w = pic.work;
            const Work::PathUnit u = w.pathUnits[w.cursor++];
            const Member& m = pic.party[u.member];
            const Spot& sp = pic.pool[static_cast<std::size_t>(u.spot)];
            PathFact* p = nullptr;
            PathTo(pic, v, s, t, m, { m.id, m.tri, sp.key, sp.tri, false }, sp.p, p);
            if (!p || !p->reachable) return;
            Work::Pair out{ u.member, u.spot, *p };
            int seen = 0;
            for (std::size_t k = 0; k < p->nAt; ++k)
                for (const std::int32_t f : m.threats)
                    if (f >= 0 && GeomSees(v, pic.foes[static_cast<std::size_t>(f)].p, p->at[k], t, s)) { ++seen; break; }
            if (p->nAt) out.exposedSec = static_cast<float>(seen) / static_cast<float>(p->nAt) * (p->length / (m.speed > 1.0f ? m.speed : 1.0f));
            w.pairs.push_back(out);
        }

        void UnitAsks(PartyPicture& pic, const ClassProfiles& profiles, const Tuning& t) {
            Work& w = pic.work;
            std::sort(w.pairs.begin(), w.pairs.end(), [](const auto& a, const auto& b) { return a.member != b.member ? a.member < b.member : a.spot < b.spot; });
            std::vector<RayAsk> want;
            const auto ask = [&](SpotKey key, const Env::Vec3& p, FormID who) {
                if (pic.facts.count({ key, who })) return;   // still fresh (Freeze dropped the stale ones)
                const RayAsk a{ key, p, who };
                if (std::find(want.begin(), want.end(), a) == want.end()) want.push_back(a);
            };
            for (const Member& m : pic.party)   // is a kept spot still good for its action: the sight-lost event
                if (m.fixed && Sighted(m.commit.act)) ask(m.commit.key, m.commit.p, m.commit.act.target);
            struct R { std::int32_t round; std::int64_t q; std::uint32_t member; std::int32_t spot; std::uint8_t action; };
            std::vector<R> rs;
            for (std::uint32_t i = 0; i < pic.party.size(); ++i) {
                const Member& m = pic.party[i];
                if (!Scores(m)) continue;
                const std::uint32_t allowed = KindsOf(pic, m, t);
                std::vector<R> mine;
                for (const auto& p : w.pairs) {
                    if (p.member != i) continue;
                    R best{ 0, -1, i, p.spot, 0 };
                    for (std::uint8_t a = 0; a < m.nTop; ++a)
                        if (const auto mask = Pairing(pic.pool[static_cast<std::size_t>(p.spot)].kinds, allowed, m.top[a])) {
                            Triple x = MakeTriple(pic, i, a, p.spot, &p, mask, profiles, t);
                            if (const auto q = Quant(ScoreTriple(pic, x, profiles, t)); q > best.q) { best.q = q; best.action = a; }
                        }
                    if (best.q > 0) mine.push_back(best);
                }
                std::sort(mine.begin(), mine.end(), [](const R& a, const R& b) { return a.q != b.q ? a.q > b.q : a.spot < b.spot; });
                for (std::size_t k = 0; k < mine.size(); ++k) { mine[k].round = static_cast<std::int32_t>(k); rs.push_back(mine[k]); }
            }
            std::sort(rs.begin(), rs.end(), [](const R& a, const R& b) { return a.round != b.round ? a.round < b.round : a.q != b.q ? a.q > b.q : a.member != b.member ? a.member < b.member : a.spot < b.spot; });
            std::set<std::int32_t> asked; std::set<std::uint32_t> stays;
            for (const R& r : rs) {
                if (!asked.count(r.spot) && asked.size() >= static_cast<std::size_t>(B.answeredSpots)) { ++pic.counts.asksShort; continue; }
                asked.insert(r.spot);
                const Member& m = pic.party[r.member];
                const Spot& sp = pic.pool[static_cast<std::size_t>(r.spot)];
                if (Sighted(m.top[r.action])) ask(sp.key, sp.p, m.top[r.action].target);
                for (const std::int32_t f : m.threats) if (f >= 0) ask(sp.key, sp.p, pic.foes[static_cast<std::size_t>(f)].id);
                // a spot that hides him is only worth something against what sees him where he stands
                if ((sp.kinds & (KindBit(SpotKind::Cover) | KindBit(SpotKind::RetreatExit))) && stays.insert(r.member).second)
                    for (const std::int32_t f : m.threats) if (f >= 0) ask(KeyOfStay(m.id), m.p, pic.foes[static_cast<std::size_t>(f)].id);
            }
            // what the last window could not serve goes first, if it is still wanted
            const auto mid = std::stable_partition(want.begin(), want.end(), [&](const RayAsk& a) { return std::find(pic.carried.begin(), pic.carried.end(), a) != pic.carried.end(); });
            pic.counts.asksCarried = static_cast<std::uint32_t>(mid - want.begin());
            const std::size_t cap = static_cast<std::size_t>(B.factsPerWindow);
            pic.carried.clear();
            if (want.size() > cap) { pic.counts.asksShort += static_cast<std::uint32_t>(want.size() - cap); pic.carried.assign(want.begin() + static_cast<std::ptrdiff_t>(cap), want.end()); want.resize(cap); }
            w.asked = want;
        }
    }

    void Freeze(PartyPicture& pic, const Env::View& v, const Tuning& t) {
        ++pic.window;
        pic.counts = {};
        pic.carried.insert(pic.carried.end(), pic.work.asked.begin(), pic.work.asked.end());   // filed, never answered
        pic.work = Work{};
        pic.pool.clear();
        const auto tidy = [](auto& list) {
            std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            list.erase(std::unique(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.id == b.id; }), list.end());
        };
        tidy(pic.party); tidy(pic.foes);
        std::erase_if(pic.foes, [](const Foe& f) { return !Finite(f.p); });
        const bool moved = pic.envStamp != v.stamp || pic.wallVert.size() != v.verts.size();
        if (moved) { pic.envStamp = v.stamp; pic.facts.clear(); pic.paths.clear(); pic.carried.clear(); WallVerts(v, pic.wallVert); }
        for (Foe& f : pic.foes) f.tri = Locate(v, f.p);
        for (Member& m : pic.party) {
            m.tri = Locate(v, m.p);
            m.nTop = (std::min)(m.nTop, static_cast<std::uint8_t>(m.top.size()));
            for (std::size_t i = 0; i < m.nTop; ++i) if (!std::isfinite(m.top[i].score) || !std::isfinite(m.top[i].reach)) m.top[i].score = m.top[i].reach = 0.0f;
        }
        // facts and paths whose ends moved, or that are too old, are gone (never used as fresh)
        std::erase_if(pic.facts, [&](const auto& e) {
            const auto& [k, f] = e;
            Env::Vec3 p; std::int32_t tri = -1;
            if (!ActorAt(pic, k.second, p, tri) || tri < 0 || tri != f.actorTri || Dist(p, f.actorAt) > t.spotFactMoveU) return true;
            if (!(pic.t - f.t >= 0.0 && pic.t - f.t <= static_cast<double>(t.spotFactMaxAgeSec))) return true;
            if (k.first & kStayBit) { const Member* m = ById(pic.party, static_cast<FormID>(k.first & 0xFFFFFFFFu)); return !m || Dist(m->p, f.spotAt) > t.spotFactMoveU; }
            return false; });
        std::erase_if(pic.paths, [&](const auto& e) {
            const Member* m = ById(pic.party, e.first.who);
            return !m || m->tri != e.first.fromTri || Dist(m->p, e.second.from) > t.spotFactMoveU; });

        for (Member& m : pic.party) {
            m.threats = { -1, -1 };
            std::vector<std::pair<std::int32_t, std::int32_t>> near;
            for (std::size_t i = 0; i < pic.foes.size(); ++i) if (pic.foes[i].target == m.id) near.push_back({ QDist(Dist(pic.foes[i].p, m.p)), static_cast<std::int32_t>(i) });
            std::sort(near.begin(), near.end());
            for (std::size_t i = 0; i < (std::min)(near.size(), m.threats.size()); ++i) m.threats[i] = near[i].second;
            m.portals = PortalsAt(v, pic.wallVert, m.tri, m.p, t);
            m.exits = m.threats[0] >= 0 ? ExitsAt(v, m.tri, m.p, pic.foes, m.who, t) : std::uint8_t{ 0 };
            Commit& c = m.commit;
            if (c.on) {
                if (moved && (c.tri < 0 || static_cast<std::size_t>(c.tri) >= v.tris.size() || Locate(v, c.p) != c.tri)) { c.on = false; m.marks |= kMarkPathBlocked; }   // the mesh under it is another one
                Env::Vec3 p; std::int32_t tri = -1;
                if (c.act.target && !ActorAt(pic, c.act.target, p, tri)) m.marks |= kMarkTargetGone;
                if (Sighted(c.act) && Sees(pic, c.key, c.act.target) == Iaus::Tri::No) m.marks |= kMarkSightLost;
                if (Dist(m.p, c.p) <= t.spotArriveU) m.marks |= kMarkArrived;
                if (m.nTop == 0 || !SameAct(m.top[0], c.held)) m.marks |= kMarkActionChanged;
                if (!(c.kinds & KindsOf(pic, m, t))) { c.on = false; m.marks |= kMarkOrder; }       // the road or the class no longer offers it
                if (m.marks & kMarkCombatEnd) c.on = false;
            }
            m.fixed = m.decides && c.on && m.marks == 0;
            if (m.decides && c.on) {
                bool in = false; for (Spot& s : pic.pool) if (s.key == c.key) { s.kinds |= c.kinds; in = true; }
                if (!in) { Spot s; s.key = c.key; s.p = c.p; s.tri = c.tri; s.kinds = c.kinds; s.portals = PortalsAt(v, pic.wallVert, c.tri, c.p, t); s.exits = ExitsAt(v, c.tri, c.p, pic.foes, m.who, t); pic.pool.push_back(s); }
            }
        }
        std::sort(pic.pool.begin(), pic.pool.end(), [](const Spot& a, const Spot& b) { return a.key < b.key; });
        if (v.tris.empty()) return;   // no view: "stay here" only

        Work& w = pic.work;
        w.stage = Work::Stage::Flood;
        w.memberFlood.resize(pic.party.size());
        std::vector<std::pair<std::int32_t, FormID>> fl;
        for (const Foe& f : pic.foes) {
            if (f.tri < 0 || f.target == 0) continue;
            float d = kInf; for (const Member& m : pic.party) if (Scores(m)) d = (std::min)(d, Dist(m.p, f.p));
            fl.push_back({ QDist(d), f.id });
        }
        std::sort(fl.begin(), fl.end());
        for (std::size_t i = 0; i < (std::min)(fl.size(), static_cast<std::size_t>(B.foeFloods)); ++i) w.foeFlood.push_back({ fl[i].second, {} });
        for (std::uint32_t i = 0; i < pic.party.size(); ++i) {
            const Member& m = pic.party[i];
            if (!Scores(m)) continue;
            const std::uint32_t allowed = KindsOf(pic, m, t);
            for (std::uint32_t a = 0; a < m.nTop; ++a)
                for (std::uint32_t k = 1; k < kSpotKindCount; ++k)
                    if ((allowed & (1u << k)) && kSpotKinds[k].gen && Pairs(static_cast<SpotKind>(k), m.top[a])) w.gen.push_back({ i, a, k });
        }
    }

    bool Phase1(PartyPicture& pic, const Env::View& v, Env::Scratch& s, const ClassProfiles& profiles, const Tuning& t, std::vector<RayAsk>& asks, float sliceMs) {
        using Stage = Work::Stage;
        asks.clear();
        Work& w = pic.work;
        if (w.stage == Stage::Done) return true;
        // The clock only decides where to pause. What is computed never depends on it.
        const auto t0 = std::chrono::steady_clock::now();
        const auto more = [&] { return sliceMs > 0.0f && std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count() < sliceMs; };
        do {
            switch (w.stage) {
            case Stage::Flood:    if (!UnitFlood(pic, v, s, t)) { w.stage = Stage::Generate; w.cursor = 0; } break;
            case Stage::Generate: if (w.cursor < w.gen.size()) UnitGenerate(pic, v, s, t); else { w.stage = Stage::Pool; w.cursor = 0; } break;
            case Stage::Pool:     UnitPool(pic, v, t); w.stage = Stage::Paths; w.cursor = 0; break;
            case Stage::Paths:    if (w.cursor < w.pathUnits.size()) UnitPath(pic, v, s, t); else { w.stage = Stage::Asks; w.cursor = 0; } break;
            case Stage::Asks:     UnitAsks(pic, profiles, t); asks = w.asked; w.stage = Stage::Done; return true;
            case Stage::Done:     return true;
            }
        } while (more());
        return false;
    }

    void Answer(PartyPicture& pic, std::span<const RayAnswer> answers) {
        for (const RayAnswer& a : answers) {
            const auto it = std::find_if(pic.work.asked.begin(), pic.work.asked.end(), [&](const RayAsk& k) { return k.spot == a.spot && k.viewer == a.viewer; });
            if (it == pic.work.asked.end()) continue;   // not a question of this window
            if (a.sees == Iaus::Tri::Unknown) continue; // not an answer: it stays asked and is carried
            PairFact f; f.sees = a.sees; f.t = pic.t; f.spotAt = it->p;
            if (ActorAt(pic, a.viewer, f.actorAt, f.actorTri)) pic.facts[{ a.spot, a.viewer }] = f;
            pic.work.asked.erase(it);
        }
    }

    void Phase3(PartyPicture& pic, const ClassProfiles& profiles, const Tuning& t, std::vector<Assignment>& out) {
        out.clear();
        const Work& w = pic.work;
        const bool done = w.stage == Work::Stage::Done;
        if (!done) pic.counts.unfinished = 1;   // a half made pool would make the result depend on the slicing: everyone who is not committed stays
        std::vector<Plan> plans(pic.party.size());
        std::vector<const Triple*> picked(pic.party.size(), nullptr);
        std::vector<std::uint8_t> open(pic.party.size(), 0);
        const auto poolIndex = [&](SpotKey k) {
            const auto it = std::lower_bound(pic.pool.begin(), pic.pool.end(), k, [](const Spot& a, SpotKey b) { return a.key < b; });
            return it != pic.pool.end() && it->key == k ? static_cast<std::int32_t>(it - pic.pool.begin()) : -1;
        };
        const auto plan = [&](std::size_t i, const Spot* s, const RankedAction* a) {
            const Member& m = pic.party[i];
            Plan& p = plans[i];
            p.set = true; p.p = s ? s->p : m.p; p.portals = s ? s->portals : m.portals; p.key = s ? s->key : KeyOfStay(m.id);
            std::int32_t tri = -1;
            p.lane = a && Sighted(*a) && a->target != m.id && ById(pic.foes, a->target) && ActorAt(pic, a->target, p.to, tri);
        };
        // committed and unmarked members, and everyone with nothing to decide, are fixed first
        for (std::size_t i = 0; i < pic.party.size(); ++i) {
            const Member& m = pic.party[i];
            if (m.fixed) { if (const auto k = poolIndex(m.commit.key); k >= 0) { plan(i, &pic.pool[static_cast<std::size_t>(k)], &m.commit.act); continue; } }
            if (!m.decides) { if (Finite(m.p)) plan(i, nullptr, nullptr); continue; }
            if (m.nTop == 0 || !Finite(m.p)) { if (Finite(m.p)) plan(i, nullptr, nullptr); continue; }
            open[i] = 1;
        }
        // the table
        std::vector<Triple> table;
        std::vector<std::pair<std::int64_t, std::size_t>> order;   // (nearness, row): which rows a full budget drops
        for (std::uint32_t i = 0; i < pic.party.size(); ++i) {
            if (!open[i]) continue;
            const Member& m = pic.party[i];
            for (std::uint8_t a = 0; a < m.nTop; ++a) table.push_back(MakeTriple(pic, i, a, -1, nullptr, KindBit(SpotKind::Stay), profiles, t));
        }
        const std::size_t stays = table.size();
        if (done) for (const auto& p : w.pairs) {
            if (!open[p.member]) continue;
            const Member& m = pic.party[p.member];
            const std::uint32_t allowed = KindsOf(pic, m, t);
            for (std::uint8_t a = 0; a < m.nTop; ++a)
                if (const auto mask = Pairing(pic.pool[static_cast<std::size_t>(p.spot)].kinds, allowed, m.top[a])) {
                    order.push_back({ QDist(p.path.length), table.size() });
                    table.push_back(MakeTriple(pic, p.member, a, p.spot, &p, mask, profiles, t));
                }
        }
        if (order.size() > static_cast<std::size_t>(B.triples)) {   // nearest first
            std::sort(order.begin(), order.end());
            pic.counts.triplesDropped = static_cast<std::uint32_t>(order.size() - static_cast<std::size_t>(B.triples));
            for (std::size_t k = static_cast<std::size_t>(B.triples); k < order.size(); ++k) table[order[k].second].dead = true;
        }
        pic.counts.triples = static_cast<std::uint32_t>(table.size() - stays);
        const auto rescore = [&](bool first) {
            for (Triple& x : table) {
                if (x.dead || !open[x.member]) continue;
                const Spot* s = x.spot >= 0 ? &pic.pool[static_cast<std::size_t>(x.spot)] : nullptr;
                if (s && Taken(*s, x.member, plans)) { x.dead = true; continue; }
                const SpotFacts before = x.f;
                FillMarks(x.f, pic, x.member, pic.party[x.member].top[x.action], s, x.pair, plans);
                if (!first && x.f == before) continue;   // the marks did not touch it
                x.score = ScoreTriple(pic, x, profiles, t); x.q = Quant(x.score);
            }
        };
        rescore(true);
        for (;;) {
            // the bar: a new spot must beat where he is going (or where he stands) by spotPreempt
            std::vector<float> stay(pic.party.size(), 0.0f), inc(pic.party.size(), -1.0f);
            for (const Triple& x : table) {
                if (x.dead || !open[x.member]) continue;
                if (x.spot < 0) stay[x.member] = (std::max)(stay[x.member], x.score);
                else if (x.incumbent) inc[x.member] = (std::max)(inc[x.member], x.score);
            }
            const Triple* best = nullptr;
            for (const Triple& x : table) {
                if (x.dead || !open[x.member]) continue;
                if (x.spot >= 0 && !x.incumbent) {
                    const float bar = inc[x.member] >= 0.0f ? inc[x.member] : stay[x.member];
                    if (!(x.score > 0.0f) || x.q <= Quant(bar * (1.0f + t.spotPreempt))) continue;
                }
                if (!best || Before(pic, x, *best)) best = &x;
            }
            if (!best) break;
            picked[best->member] = best;
            open[best->member] = 0;
            plan(best->member, best->spot >= 0 ? &pic.pool[static_cast<std::size_t>(best->spot)] : nullptr, &pic.party[best->member].top[best->action]);
            rescore(false);
        }
        for (std::size_t i = 0; i < pic.party.size(); ++i) {
            const Member& m = pic.party[i];
            if (!m.decides) continue;
            Assignment a; a.who = m.id; a.p = m.p; a.tri = m.tri; a.key = KeyOfStay(m.id); a.kinds = KindBit(SpotKind::Stay);
            const auto spotOf = [&](std::int32_t k) { const Spot& s = pic.pool[static_cast<std::size_t>(k)]; a.spot = k; a.key = s.key; a.p = s.p; a.tri = s.tri; };
            if (m.fixed) {
                if (const auto k = poolIndex(m.commit.key); k >= 0) spotOf(k);
                a.act = m.commit.act; a.kinds = m.commit.kinds; a.kept = true;
            } else if (const Triple* x = picked[i]) {
                a.act = m.top[x->action]; a.score = x->score;
                if (x->spot >= 0) { spotOf(x->spot); a.kinds = Pairing(pic.pool[static_cast<std::size_t>(x->spot)].kinds, KindsOf(pic, m, t), a.act); a.kept = x->incumbent; }
            }
            a.action = a.act.family; a.form = a.act.form; a.target = a.act.target;
            out.push_back(a);
        }
    }

    void Adopt(PartyPicture& pic, std::span<const Assignment> as) {
        for (const Assignment& a : as) {
            Member* m = ById(pic.party, a.who);
            if (!m) continue;
            if (a.spot < 0) m->commit = Commit{};
            else if (!m->fixed) {
                const double at = a.kept ? m->commit.at : pic.t;
                m->commit = Commit{ true, a.key, a.p, a.tri, a.kinds, a.act, m->top[0], at };
            }
            m->marks = 0;
        }
    }
}
