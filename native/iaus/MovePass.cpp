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
        // A fact whose end moved, or that is too old, is never used as fresh.
        bool Stale(const PartyPicture& pic, const PairKey& k, const PairFact& f, double now, const Tuning& t) {
            Env::Vec3 p; std::int32_t tri = -1;
            if (!ActorAt(pic, k.second, p, tri) || tri < 0 || (f.actorTri != kAnyTri && tri != f.actorTri) || !(Dist(p, f.actorAt) <= t.spotFactMoveU)) return true;
            if (!(now - f.t >= 0.0 && now - f.t <= static_cast<double>(t.spotFactMaxAgeSec))) return true;
            if (k.first & kStayBit) { const Member* m = ById(pic.party, static_cast<FormID>(k.first & 0xFFFFFFFFu)); return !m || !(Dist(m->p, f.spotAt) <= t.spotFactMoveU); }
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

        Env::Vec3 Between(const Env::Vec3& a, const Env::Vec3& b, float k) { return { a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k }; }
        float LitShare(const PartyPicture& pic, const Env::Vec3& p, const Tuning& t) {   // 0 = dark or not known, 1 = lit
            const auto l = Light::At(pic.lightField, p, pic.t, t);
            return l.Known() && t.lightLitLevel > 0.0f ? (std::min)(1.0f, l.level / t.lightLitLevel) : 0.0f;
        }

        // THE ONE FILLER of SpotFacts. Static: what no other member's choice changes. a_pair / a_spot null = stay here.
        SpotFacts FillStatic(const PartyPicture& pic, const Member& m, const RankedAction& a, const Spot* spot, const Work::Pair* pair, const Tuning& t) {
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
                if (spot) {
                    f.sees = Sees(pic, key, a.target);
                    // the answer that marked him may be older than a fact lives (the foe ran on): the spot he is freed FROM is not "unknown" again
                    // he stands on it (inside the claim's radius) and HIS OWN ray says no: the point may see, he does not
                    if ((m.seen & kMarkArrived) && m.commit.on && m.commit.key == key && a.sight == Sight::Occluded) f.sees = Iaus::Tri::No;
                    if (f.sees == Iaus::Tri::Unknown && (m.seen & kMarkSightLost) && m.commit.on && m.commit.key == key && m.commit.act.target == a.target) f.sees = Iaus::Tri::No;
                }
                else f.sees = a.sight == Sight::Visible ? Iaus::Tri::Yes : a.sight == Sight::Occluded ? Iaus::Tri::No : Iaus::Tri::Unknown;   // the action facet's own ray
            }
            if (hasTarget && a.reach > 0.0f) {
                const float d = Dist(at, tp);
                f.rangeX = a.melee ? (d <= a.reach ? 0.5f : 1.0f + (d - a.reach) / 3000.0f) : d / a.reach;   // melee closes by itself: a shade, not a band
            }
            if (foe && a.melee) if (const auto it = pic.work.reach.find({ m.id, a.target }); it != pic.work.reach.end()) f.foeReachable = it->second;
            int threats = 0; float seen = 0.0f;
            for (const std::int32_t i : m.threats) if (i >= 0) {
                ++threats;
                const auto s = Sees(pic, key, pic.foes[static_cast<std::size_t>(i)].id);
                seen += s == Iaus::Tri::Yes ? 1.0f : s == Iaus::Tri::No ? 0.0f : t.spotUnknownExposure;
            }
            if (threats) f.exposure = seen / static_cast<float>(threats);
            if (threats) { f.pathExposedSec = pair ? pair->exposedSec : 0.0f; f.exits = static_cast<float>(spot ? spot->exits : m.exits); }
            if (!pic.areas.empty()) {
                f.inHostileArea = Iaus::Tri::No;
                for (const Area& r : pic.areas) if (Dist(at, r.p) < r.r) f.inHostileArea = Iaus::Tri::Yes;
            }
            if (m.leash > 0.0f) f.leashX = Dist(at, m.anchor) / m.leash;
            if (m.wantsDark) {   // S4. Unknown stays NOT APPLICABLE: never dark
                for (const LightAt& l : pic.light) if (l.key == key && l.level >= 0.0f) f.light = l.level;
                if (f.light < 0.0f) f.light = Light::At(pic.lightField, at, pic.t, t).level;
                if (pair && pair->path.nVia) {   // the walk as the engine would take it: the claim names a place, not a way
                    int known = 0, lit = 0;
                    for (std::size_t i = 0; i < pair->path.nVia; ++i) if (const auto r = Light::At(pic.lightField, pair->path.via[i], pic.t, t); r.Known()) { ++known; lit += r.level >= t.lightLitLevel; }
                    if (known) f.pathLitSec = f.pathSec * static_cast<float>(lit) / static_cast<float>(pair->path.nVia);
                }
                if (f.light >= 0.0f && m.sneakSkill >= 0.0f) {   // the engine's own formula, crouched and still there, per foe that may see the place
                    float worst = -1.0f;
                    for (const Foe& e : pic.foes) {
                        if (e.skill < 0.0f) continue;
                        Light::DetectIn in; in.dist = Dist(e.p, at); in.exterior = pic.exterior; in.targetIsOsTarget = e.target == m.id; in.oHasTarget = e.target != 0;
                        in.skillO = e.skill; in.skillT = m.sneakSkill; in.sneakingT = true; in.invisible = m.invisible; in.light = f.light;
                        const float dx = at.x - e.p.x, dy = at.y - e.p.y, d = std::sqrt(dx * dx + dy * dy);
                        if (d > 1.0f && (e.faceX != 0.0f || e.faceY != 0.0f)) in.angleDeg = std::acos((std::clamp)((e.faceX * dx + e.faceY * dy) / d, -1.0f, 1.0f)) * 57.29578f;   // unknown = straight ahead
                        const auto s = Sees(pic, key, e.id); in.losGeom = s != Iaus::Tri::No;
                        const int level = Light::DetectLevel(pic.detect, in).level;   // a whole number: a last bit of libm does not reach the rank
                        const float x = level > 0 ? (std::min)(1.0f, static_cast<float>(level) / (std::max)(1.0f, pic.detect.barSneak)) : 0.0f;
                        worst = (std::max)(worst, s == Iaus::Tri::Unknown ? x * t.spotUnknownExposure : x);
                    }
                    f.detected = worst;
                }
            }
            if (spot && !pic.trustNow.Clean()) {   // where he stands now is nobody's plan: staying hands him back to the engine, and is never priced
                if (const auto l = Trust::Of(pic.trustNow, spot->p); l != Trust::Level::Trusted) f.spotRegionTrust = static_cast<float>(l);
                if (pair) if (const auto l = Trust::RouteLevel(pic.trustNow, m.p, { pair->path.via.data(), pair->path.nVia }); l != Trust::Level::Trusted) f.pathRegionTrust = static_cast<float>(l);
            }
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

        struct Triple { std::uint32_t member; std::uint8_t action; std::int32_t spot; std::uint32_t kinds; const Work::Pair* pair; float scale; SpotFacts f; float score = 0.0f; std::int64_t q = 0; bool dead = false, incumbent = false; };

        float ScoreTriple(const PartyPicture& pic, Triple& x, const ClassProfiles& profiles, const Tuning& t) {
            const Member& m = pic.party[x.member];
            const RankedAction& a = m.top[x.action];
            const float s = a.score * x.scale * ScoreSpot(x.f, a.Kind(), profiles(m.classId), t);
            return std::isfinite(s) && s > 0.0f ? s : 0.0f;
        }
        Triple MakeTriple(const PartyPicture& pic, std::uint32_t mi, std::uint8_t ai, std::int32_t si, const Work::Pair* pair, std::uint32_t mask, const ClassProfiles& profiles, const Tuning& t) {
            const Member& m = pic.party[mi];
            const Spot* s = si >= 0 ? &pic.pool[static_cast<std::size_t>(si)] : nullptr;
            Triple x{ mi, ai, si, mask, pair, 1.0f, FillStatic(pic, m, m.top[ai], s, pair, t) };
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
            const bool any = Env::FindPath(v, { m.p, to, m.tri, k.toTri, m.who, pic.avoid }, s, p);
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
                const int nv = (std::min)(static_cast<int>(f.via.size()), 1 + static_cast<int>(p.length / 128.0f));   // the walk in equal pieces
                prev = m.p; done = 0.0f; next = 1;
                for (const auto& c : p.pts) {
                    const float leg = Dist(prev, c.p);
                    while (next < nv && leg > 0.0f && p.length * static_cast<float>(next) <= (done + leg) * static_cast<float>(nv)) {
                        const float at = (p.length * static_cast<float>(next) / static_cast<float>(nv) - done) / leg;
                        f.via[f.nVia++] = { prev.x + (c.p.x - prev.x) * at, prev.y + (c.p.y - prev.y) * at, prev.z + (c.p.z - prev.z) * at };
                        ++next;
                    }
                    done += leg; prev = c.p;
                }
                f.via[f.nVia++] = to;
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

        // His walk, or last window's when he has not moved at all (a flood reads the mesh, the doors, the point and WalkerBits).
        PartyPicture::Still& StillOf(PartyPicture& pic, FormID who, const Env::Vec3& p, std::int32_t tri, const Env::Walker& w) {
            PartyPicture::Still* s = nullptr;
            for (auto& e : pic.still) if (e.who == who) { s = &e; break; }
            if (!s) { pic.still.push_back({}); s = &pic.still.back(); s->who = who; }
            if (s->tri != tri || s->p.x != p.x || s->p.y != p.y || s->p.z != p.z || s->bits != WalkerBits(w)) { *s = {}; s->who = who; s->p = p; s->tri = tri; s->bits = WalkerBits(w); }
            return *s;
        }
        std::shared_ptr<const Work::Flood> WalkOf(PartyPicture& pic, const Env::View& v, FormID who, const Env::Vec3& p, std::int32_t tri, const Env::Walker& w, float radius) {
            PartyPicture::Still& s = StillOf(pic, who, p, tri, w);
            if (!s.flood || s.radius != radius) { auto f = std::make_shared<Work::Flood>(); f->who = who; Flood(v, p, tri, w, radius, B.gatherTris, *f, pic.mesh.get()); s.flood = f; s.radius = radius; }
            return s.flood;
        }
        std::uint8_t OwnExits(PartyPicture& pic, const Env::View& v, const Member& m, const Tuning& t) {
            PartyPicture::Still& s = StillOf(pic, m.id, m.p, m.tri, m.who);
            if (s.exitU != t.spotExitU) { s.open = OpenOctants(v, m.tri, m.p, m.who, t, pic.mesh.get()); s.exitU = t.spotExitU; }
            return ExitsOf(s.open, m.p, pic.foes, t);
        }
        // A spot's ways out: the mesh's half is worked out once per spot (only for the exact point of its key, and the plain walker's bits).
        std::uint8_t SpotExits(PartyPicture& pic, const Env::View& v, SpotKey key, std::int32_t tri, const Env::Vec3& p, const Env::Walker& w, const Tuning& t) {
            const auto plain = [&] {
                if (key != KeyOfPoint(tri, static_cast<int>(key & 3)) || tri < 0 || static_cast<std::size_t>(tri) >= pic.mesh->cen.size() || WalkerBits(w) != WalkerBits(Env::Walker{})) return false;
                const Env::Vec3 c = pic.mesh->cen[static_cast<std::size_t>(tri)]; Env::Vec3 q = c;
                if (key & 3) { const Env::Vec3& k = v.verts[v.tris[static_cast<std::size_t>(tri)].v[(key & 3) - 1]]; q = { 0.5f * (c.x + k.x), 0.5f * (c.y + k.y), 0.5f * (c.z + k.z) }; }
                return q.x == p.x && q.y == p.y && q.z == p.z; };
            if (!plain()) return ExitsOf(OpenOctants(v, tri, p, w, t, pic.mesh.get()), p, pic.foes, t);
            const auto slot = static_cast<std::size_t>(key);
            if (!pic.exitMemo || pic.exitMemo->exitU != t.spotExitU || pic.exitMemo->open.size() != v.tris.size() * 4) {
                pic.exitMemo = std::make_shared<ExitMemo>(); pic.exitMemo->exitU = t.spotExitU; pic.exitMemo->open.assign(v.tris.size() * 4, 0xFFFF); }
            if (pic.exitMemo->open[slot] == 0xFFFF) {
                const std::uint8_t o = OpenOctants(v, tri, p, w, t, pic.mesh.get());
                if (pic.exitMemo.use_count() > 1) pic.exitMemo = std::make_shared<ExitMemo>(*pic.exitMemo);   // a copy of the picture reads it: never written under it
                pic.exitMemo->open[slot] = o;
            }
            return ExitsOf(static_cast<std::uint8_t>(pic.exitMemo->open[slot]), p, pic.foes, t);
        }

        // One flood (a member's with the reach of his foes, then the foes'). False = none left.
        bool UnitFlood(PartyPicture& pic, const Env::View& v, Env::Scratch& s, const Tuning& t) {
            Work& w = pic.work;
            const std::size_t i = w.cursor++;
            if (i < pic.party.size()) {
                const Member& m = pic.party[i];
                if (!Scores(m)) return true;
                w.memberFlood[i] = { m.id, WalkOf(pic, v, m.id, m.p, m.tri, m.who, t.spotRadius) };
                if (w.memberFlood[i].f->cut) ++pic.counts.floodsShort;
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
            if (const Foe* f = ById(pic.foes, w.foeFlood[j].who)) {
                w.foeFlood[j].f = WalkOf(pic, v, f->id, f->p, f->tri, Env::Walker{}, t.spotRadius);
                if (w.foeFlood[j].f->cut) ++pic.counts.floodsShort;
            }
            return true;
        }

        GenIn MakeGen(const PartyPicture& pic, const Env::View& v, const Tuning& t, std::uint32_t mi, std::uint32_t ai) {
            const Member& m = pic.party[mi];
            const RankedAction& a = m.top[ai];
            GenIn g{ pic, v, t, m, a, pic.work.memberFlood[mi].Get(), {} };
            std::int32_t tt = -1;
            g.hasTarget = a.target != 0 && a.target != m.id && ActorAt(pic, a.target, g.target, tt);
            g.foe = g.hasTarget ? ById(pic.foes, a.target) : nullptr;
            g.targetIsAlly = g.hasTarget && !g.foe;
            if (const auto it = pic.work.reach.find({ m.id, a.target }); it != pic.work.reach.end()) g.targetUnreachable = it->second == Iaus::Tri::No;
            return g;
        }
        float WalkTo(const PartyPicture& pic, const Member& m, const Work::Flood& fl, const Env::Vec3& p, std::int32_t tri) {   // < 0: beyond his gather
            if (tri == m.tri) return Dist(m.p, p);
            const float d = fl.At(tri);
            return d < 0.0f ? -1.0f : d + Dist(pic.mesh->cen[static_cast<std::size_t>(tri)], p);
        }

        void UnitGenerate(PartyPicture& pic, const Env::View& v, Env::Scratch& s, const Tuning& t) {
            Work& w = pic.work;
            const Work::GenUnit u = w.gen[w.cursor++];
            const Member& m = pic.party[u.member];
            const SpotKindRow& row = kSpotKinds[u.kind];
            const GenIn g = MakeGen(pic, v, t, u.member, u.action);
            struct R { std::int32_t q; Cand c; };
            if (w.points.size() < pic.party.size()) w.points.resize(pic.party.size());
            std::vector<Cand>& pts = w.points[u.member];
            if (pts.empty())
                for (const auto& [tri, d] : g.own.d) {
                    const std::size_t from = pts.size();
                    Points(v, tri, t, pts, pic.mesh.get());
                    const Env::Vec3 c = pic.mesh->cen[static_cast<std::size_t>(tri)];
                    for (std::size_t i = from; i < pts.size(); ++i) {
                        pts[i].walk = tri == m.tri ? Dist(m.p, pts[i].p) : d + Dist(c, pts[i].p);
                        if (m.wantsDark && !pic.lightField.samples.empty())   // only the best few are kept or tested: a lit place must not crowd a dark one out. Unknown adds nothing
                            pts[i].lit = t.lightGenU * (LitShare(pic, pts[i].p, t) + (LitShare(pic, Between(m.p, pts[i].p, 0.25f), t) + LitShare(pic, Between(m.p, pts[i].p, 0.5f), t) + LitShare(pic, Between(m.p, pts[i].p, 0.75f), t)) / 3.0f);   // the place, and the straight way to it
                    }
                }
            thread_local std::vector<R> found; found.clear();
            // a place his leg was refused at is not looked at, so it takes no room of the sorted part below (S3 review R1)
            for (const Cand& k : pts) { float r = 0.0f; if (row.gen(g, k, r) && std::isfinite(r) && !Trust::Refused(pic.trustNow, m.id, k.key)) found.push_back({ QDist(r + k.lit), k }); }
            // only the first geomPerGen are ever looked at, and the order is total (a key is one point)
            const auto upTo = found.begin() + static_cast<std::ptrdiff_t>((std::min)(found.size(), static_cast<std::size_t>((std::max)(B.geomPerGen, B.perGen))));
            std::partial_sort(found.begin(), upTo, found.end(), [](const R& x, const R& y) { return x.q != y.q ? x.q < y.q : x.c.key < y.c.key; });
            int kept = 0, tested = 0;
            for (const R& x : found) {
                if (kept >= B.perGen) break;
                if (row.check) { if (tested++ >= B.geomPerGen) break; if (!row.check(g, x.c, s)) continue; }
                Spot sp; sp.key = x.c.key; sp.p = x.c.p; sp.tri = x.c.tri; sp.kinds = KindBit(row.kind);
                w.hits.push_back({ sp, kept * 100000 + (std::min)(99999, QDist(x.c.walk)) });   // the first of every kind before the second of any, nearest first
                ++kept;
            }
        }

        // The pool, in units: one per spot where the cost is per spot (the exits flood, the offers), one for the sorts.
        void UnitMerge(PartyPicture& pic) {
            Work& w = pic.work;
            std::map<SpotKey, Work::Hit> all;
            for (const Work::Hit& h : w.hits) {
                auto [it, fresh] = all.try_emplace(h.s.key, h);
                if (!fresh) { it->second.s.kinds |= h.s.kinds; it->second.rank = (std::min)(it->second.rank, h.rank); }
            }
            pic.counts.generated = static_cast<std::uint32_t>(all.size());
            w.hits.clear();
            for (const auto& [key, h] : all) w.hits.push_back(h);
        }
        void UnitPlace(PartyPicture& pic, const Env::View& v, const Tuning& t) {
            Spot& s = pic.work.hits[pic.work.cursor++].s;
            s.portals = PortalsAt(v, pic.wallVert, s.tri, s.p, t);
            s.exits = SpotExits(pic, v, s.key, s.tri, s.p, Env::Walker{}, t);
            if (s.exits == 0) s.kinds &= ~KindBit(SpotKind::RetreatExit);   // a dead end is not offered
        }
        void UnitSelect(PartyPicture& pic) {
            std::vector<Work::Hit> list;
            for (const Work::Hit& h : pic.work.hits) {
                bool in = false;   // a committed spot is in the pool already (Freeze): it gains the kinds found this window
                for (Spot& p : pic.pool) if (p.key == h.s.key) { p.kinds |= h.s.kinds; in = true; }
                if (!in && h.s.kinds) list.push_back(h);
            }
            std::sort(list.begin(), list.end(), [](const Work::Hit& a, const Work::Hit& b) { return a.rank != b.rank ? a.rank < b.rank : a.s.key < b.s.key; });
            const std::size_t room = static_cast<std::size_t>((std::max)(0, B.spots - static_cast<int>(pic.pool.size())));
            if (list.size() > room) { pic.counts.spotsDropped += static_cast<std::uint32_t>(list.size() - room); list.resize(room); }
            for (const auto& h : list) pic.pool.push_back(h.s);
            std::sort(pic.pool.begin(), pic.pool.end(), [](const Spot& a, const Spot& b) { return a.key < b.key; });
        }
        // One pooled spot: what is it for each member and action that could use it, by the row's own rule (Work::Offer)?
        void UnitOffer(PartyPicture& pic, const Env::View& v, Env::Scratch& s, const Tuning& t) {
            Work& w = pic.work;
            const auto si = static_cast<std::int32_t>(w.cursor++);
            const Spot& sp = pic.pool[static_cast<std::size_t>(si)];
            std::map<FormID, bool> sight;   // CheckSight reads the target and the spot only
            for (std::uint32_t i = 0; i < pic.party.size(); ++i) {
                const Member& m = pic.party[i];
                if (!Scores(m) || Trust::Refused(pic.trustNow, m.id, sp.key)) continue;   // his last leg there was refused
                const bool own = m.commit.on && m.commit.key == sp.key;
                Cand c{ sp.key, sp.p, sp.tri, WalkTo(pic, m, w.memberFlood[i].Get(), sp.p, sp.tri) };
                if (c.walk < 0.0f) { if (!own) continue; c.walk = Dist(m.p, sp.p); }   // beyond his own gather: another member's spot
                const std::uint32_t allowed = KindsOf(pic, m, t);
                for (std::uint8_t ai = 0; ai < m.nTop; ++ai) {
                    const RankedAction& a = m.top[ai];
                    const GenIn g = MakeGen(pic, v, t, i, ai);
                    const bool same = own && a.family == m.commit.act.family && a.target == m.commit.act.target;
                    std::uint32_t mask = 0;
                    for (std::size_t k = 1; k < kSpotKindCount; ++k) {
                        const SpotKindRow& row = kSpotKinds[k];
                        if (!(sp.kinds & allowed & (1u << k)) || !row.gen || !Pairs(row.kind, a)) continue;
                        if (same && (m.commit.kinds & (1u << k))) { if (KeptHolds(row.kind, g, c)) mask |= 1u << k; continue; }
                        float r = 0.0f;
                        if (!row.gen(g, c, r) || !std::isfinite(r)) continue;
                        if (row.check == &CheckSight) {
                            const auto [it, fresh] = sight.try_emplace(a.target, false);
                            if (fresh) it->second = CheckSight(g, c, s);
                            if (!it->second) continue;
                        } else if (row.check && !row.check(g, c, s)) continue;
                        mask |= 1u << k;
                    }
                    if (mask) w.offers.push_back({ si, i, ai, mask });
                }
            }
        }

        void UnitQueue(PartyPicture& pic) {
            Work& w = pic.work;
            // who may walk where, each member's nearest first, one round at a time
            struct U { std::int32_t round; Work::PathUnit u; };
            std::vector<U> us;
            for (std::uint32_t i = 0; i < pic.party.size(); ++i) {
                const Member& m = pic.party[i];
                if (!Scores(m)) continue;
                std::vector<Work::PathUnit> mine;
                for (std::size_t k = 0; k < pic.pool.size(); ++k) {
                    const Spot& sp = pic.pool[k];
                    const bool own = m.commit.on && m.commit.key == sp.key;
                    std::uint32_t any = 0;
                    for (std::uint8_t ai = 0; ai < m.nTop; ++ai) any |= w.Offered(static_cast<std::int32_t>(k), i, ai);
                    if (!any) continue;
                    mine.push_back({ i, static_cast<std::int32_t>(k), 0, own ? -1 : QDist(WalkTo(pic, m, w.memberFlood[i].Get(), sp.p, sp.tri)) });
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
                    if (f >= 0 && GeomSees(v, pic.foes[static_cast<std::size_t>(f)].p, p->at[k], t, s, pic.sight)) { ++seen; break; }
            if (p->nAt) out.exposedSec = static_cast<float>(seen) / static_cast<float>(p->nAt) * (p->length / (m.speed > 1.0f ? m.speed : 1.0f));
            w.pairs.push_back(out);
        }

        // What phase 1 alone needed. A picture is copied whole (the lab's rollouts), and phase 3 reads the pool, the offers, the pairs and reach.
        void Spent(Work& w) { w.memberFlood.clear(); w.foeFlood.clear(); w.points.clear(); w.hits.clear(); w.gen.clear(); w.pathUnits.clear(); }

        void UnitAsks(PartyPicture& pic, const ClassProfiles& profiles, const Tuning& t) {
            Work& w = pic.work;
            std::sort(w.pairs.begin(), w.pairs.end(), [](const auto& a, const auto& b) { return a.member != b.member ? a.member < b.member : a.spot < b.spot; });
            std::vector<RayAsk> want;
            const auto ask = [&](SpotKey key, const Env::Vec3& p, FormID who, float maxAge) {
                if (const auto it = pic.facts.find({ key, who }); it != pic.facts.end() && pic.t - it->second.t < static_cast<double>(maxAge)) return;   // still fresh (Freeze dropped the stale ones)
                RayAsk a{ key, p, who, pic.t, {}, -1 };
                if (!ActorAt(pic, who, a.viewerAt, a.viewerTri)) return;
                if (std::find(want.begin(), want.end(), a) == want.end()) want.push_back(a);
            };
            for (const Member& m : pic.party)   // is a kept spot still good for its action: the sight-lost event
                if (m.fixed && Sighted(m.commit.act)) ask(m.commit.key, m.commit.p, m.commit.act.target, t.spotKeptAgeSec);
            const auto kept = static_cast<std::ptrdiff_t>(want.size());
            struct R { std::int32_t round; std::int64_t q; std::uint32_t member; std::int32_t spot; std::uint8_t action; };
            std::vector<R> rs;
            for (std::uint32_t i = 0; i < pic.party.size(); ++i) {
                const Member& m = pic.party[i];
                if (!Scores(m)) continue;
                std::vector<R> mine;
                for (const auto& p : w.pairs) {
                    if (p.member != i) continue;
                    R best{ 0, -1, i, p.spot, 0 };
                    for (std::uint8_t a = 0; a < m.nTop; ++a)
                        if (const auto mask = w.Offered(p.spot, i, a)) {
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
                if (Sighted(m.top[r.action])) ask(sp.key, sp.p, m.top[r.action].target, kInf);
                for (const std::int32_t f : m.threats) if (f >= 0) ask(sp.key, sp.p, pic.foes[static_cast<std::size_t>(f)].id, kInf);
                // a spot is only worth something against what sees him where he stands
                if (stays.insert(r.member).second)
                    for (const std::int32_t f : m.threats) if (f >= 0) ask(KeyOfStay(m.id), m.p, pic.foes[static_cast<std::size_t>(f)].id, kInf);
            }
            // the kept spots' questions first (at most one a member), then what the last window could not serve, if it is still wanted
            const auto mid = std::stable_partition(want.begin() + kept, want.end(), [&](const RayAsk& a) { return std::find(pic.carried.begin(), pic.carried.end(), a) != pic.carried.end(); });
            pic.counts.asksCarried = static_cast<std::uint32_t>(mid - want.begin() - kept);
            const std::size_t cap = static_cast<std::size_t>(B.factsPerWindow);
            pic.carried.clear();
            if (want.size() > cap) { pic.counts.asksShort += static_cast<std::uint32_t>(want.size() - cap); pic.carried.assign(want.begin() + static_cast<std::ptrdiff_t>(cap), want.end()); want.resize(cap); }
            w.asked = want;
        }
    }

    void File(PartyPicture& pic) {
        for (const RayAsk& a : pic.work.asked) {   // filed, never answered
            pic.carried.push_back(a);
            if (std::find(pic.late.begin(), pic.late.end(), a) == pic.late.end()) pic.late.push_back(a);   // the first filing is the one that counts
        }
        pic.work.asked.clear();
    }

    void Freeze(PartyPicture& pic, const Env::View& v, const Tuning& t) {
        ++pic.window;
        pic.counts = {};
        File(pic);
        pic.work = Work{};
        pic.pool.clear();
        const auto tidy = [](auto& list) {
            std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            list.erase(std::unique(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.id == b.id; }), list.end());
        };
        tidy(pic.party); tidy(pic.foes);
        std::erase_if(pic.foes, [](const Foe& f) { return !Finite(f.p); });
        const bool moved = pic.envStamp != v.stamp || pic.wallVert.size() != v.verts.size();
        if (moved) { pic.envStamp = v.stamp; pic.facts.clear(); pic.paths.clear(); pic.carried.clear(); pic.late.clear(); WallVerts(v, pic.wallVert); }
        if (moved || !pic.mesh || pic.mesh->cen.size() != v.tris.size()) { pic.mesh = FactsOf(v); pic.exitMemo.reset(); pic.exitFlood.reset(); pic.still.clear(); }
        std::erase_if(pic.still, [&](const auto& s) { return !ById(pic.party, s.who) && !ById(pic.foes, s.who); });
        if (moved) Trust::Forget(pic.trust);           // a door, a cell: what was learned is about another mesh
        Trust::Decay(pic.trust, pic.t);
        Light::Decay(pic.lightField, pic.t, t);
        pic.trustNow = pic.trust; pic.trustNow.log.clear(); pic.trustNow.said.clear();
        if (moved || pic.trustGen != pic.trust.gen) {
            pic.trustGen = pic.trust.gen; pic.paths.clear(); pic.avoid.clear();
            if (!pic.trustNow.Clean()) {
                std::map<Trust::Region, float> price;
                pic.avoid.assign(v.tris.size(), 1.0f);
                for (std::size_t i = 0; i < v.tris.size(); ++i) {
                    const auto r = Trust::RegionOf(pic.mesh->cen[i], pic.trustNow.rules);
                    const auto [it, fresh] = price.try_emplace(r, 1.0f);
                    if (fresh) { const auto l = Trust::Of(pic.trustNow, r); it->second = l == Trust::Level::Distrusted ? pic.trustNow.rules.distrustMult : l == Trust::Level::Doubted ? pic.trustNow.rules.doubtMult : 1.0f; }
                    pic.avoid[i] = it->second;
                }
            }
        }
        for (Foe& f : pic.foes) f.tri = Locate(v, f.p);
        for (Member& m : pic.party) {
            m.tri = Locate(v, m.p);
            m.nTop = (std::min)(m.nTop, static_cast<std::uint8_t>(m.top.size()));
            for (std::size_t i = 0; i < m.nTop; ++i) if (!std::isfinite(m.top[i].score) || !std::isfinite(m.top[i].reach)) m.top[i].score = m.top[i].reach = 0.0f;
        }
        const auto given = pic.late.size();
        std::erase_if(pic.late, [&](const RayAsk& a) { return !(pic.t - a.t >= 0.0 && pic.t - a.t <= static_cast<double>(t.spotFactMaxAgeSec)); });
        if (pic.late.size() > static_cast<std::size_t>(B.lateAsks)) pic.late.erase(pic.late.begin(), pic.late.end() - B.lateAsks);
        pic.counts.lateDropped = static_cast<std::uint32_t>(given - pic.late.size());
        // the holder's inbox, and what this freeze finds. A window that was never adopted keeps its marks. Sight first: a runner's fact dies in the next lines
        for (Member& m : pic.party) {
            m.seen |= m.marks; m.marks = 0;
            if (m.commit.on && Sighted(m.commit.act) && Sees(pic, m.commit.key, m.commit.act.target) == Iaus::Tri::No) m.seen |= kMarkSightLost;
        }
        // facts and paths whose ends moved, or that are too old, are gone (never used as fresh)
        std::erase_if(pic.facts, [&](const auto& e) { return Stale(pic, e.first, e.second, pic.t, t); });
        std::erase_if(pic.paths, [&](const auto& e) {
            const Member* m = ById(pic.party, e.first.who);
            if (!m || m->tri != e.first.fromTri || Dist(m->p, e.second.from) > t.spotFactMoveU) return true;
            if (!e.first.actor) return false;
            const Foe* f = ById(pic.foes, static_cast<FormID>(e.first.to));   // a walk to where a foe no longer stands
            return !f || f->tri != e.first.toTri; });

        for (Member& m : pic.party) {
            m.threats = { -1, -1 };
            std::vector<std::pair<std::int32_t, std::int32_t>> near;
            for (std::size_t i = 0; i < pic.foes.size(); ++i) if (pic.foes[i].target == m.id) near.push_back({ QDist(Dist(pic.foes[i].p, m.p)), static_cast<std::int32_t>(i) });
            std::sort(near.begin(), near.end());
            for (std::size_t i = 0; i < (std::min)(near.size(), m.threats.size()); ++i) m.threats[i] = near[i].second;
            m.portals = PortalsAt(v, pic.wallVert, m.tri, m.p, t);
            m.exits = m.threats[0] >= 0 ? OwnExits(pic, v, m, t) : std::uint8_t{ 0 };
            Commit& c = m.commit;
            if (c.on) {
                if (moved && (c.tri < 0 || static_cast<std::size_t>(c.tri) >= v.tris.size() || Locate(v, c.p) != c.tri)) { c.on = false; m.seen |= kMarkPathBlocked; }   // the mesh under it is another one
                Env::Vec3 p; std::int32_t tri = -1;
                if (c.act.target && !ActorAt(pic, c.act.target, p, tri)) m.seen |= kMarkTargetGone;
                if (Dist(m.p, c.p) <= t.spotArriveU) m.seen |= kMarkArrived;
                if (m.nTop == 0 || !SameAct(m.top[0], c.held)) m.seen |= kMarkActionChanged;
                if (!(c.kinds & KindsOf(pic, m, t))) { c.on = false; m.seen |= kMarkOrder; }       // the road or the class no longer offers it
                if (Trust::Refused(pic.trustNow, m.id, c.key)) { c.on = false; m.seen |= kMarkPathBlocked; }
                if (m.seen & kMarkCombatEnd) c.on = false;
            }
            m.fixed = m.decides && c.on && m.seen == 0;
            if (m.decides && c.on) {
                bool in = false; for (Spot& s : pic.pool) if (s.key == c.key) { s.kinds |= c.kinds; in = true; }
                if (!in) { Spot s; s.key = c.key; s.p = c.p; s.tri = c.tri; s.kinds = c.kinds; s.portals = PortalsAt(v, pic.wallVert, c.tri, c.p, t); s.exits = SpotExits(pic, v, c.key, c.tri, c.p, m.who, t); pic.pool.push_back(s); }
            }
        }
        std::sort(pic.pool.begin(), pic.pool.end(), [](const Spot& a, const Spot& b) { return a.key < b.key; });
        if (v.tris.empty()) return;   // no view: "stay here" only

        Work& w = pic.work;
        w.stage = Work::Stage::Flood;
        w.memberFlood.resize(pic.party.size());
        bool foesWalk = false;   // a foe's own walk is read by kite and by retreat's way on, and both need somebody on him
        for (std::uint32_t i = 0; i < pic.party.size(); ++i) {
            const Member& m = pic.party[i];
            if (!Scores(m)) continue;
            const std::uint32_t allowed = KindsOf(pic, m, t);
            for (std::uint32_t a = 0; a < m.nTop; ++a)
                for (std::uint32_t k = 1; k < kSpotKindCount; ++k)
                    if ((allowed & (1u << k)) && kSpotKinds[k].gen && Pairs(static_cast<SpotKind>(k), m.top[a])) { w.gen.push_back({ i, a, k }); foesWalk = foesWalk || (m.threats[0] >= 0 && (k == static_cast<std::uint32_t>(SpotKind::Kite) || k == static_cast<std::uint32_t>(SpotKind::RetreatExit))); }
        }
            if (!foesWalk) return;
        std::vector<std::pair<std::int32_t, FormID>> fl;
        for (const Foe& f : pic.foes) {
            if (f.tri < 0 || f.target == 0) continue;
            float d = kInf; for (const Member& m : pic.party) if (Scores(m)) d = (std::min)(d, Dist(m.p, f.p));
            fl.push_back({ QDist(d), f.id });
        }
        std::sort(fl.begin(), fl.end());
        for (std::size_t i = 0; i < (std::min)(fl.size(), static_cast<std::size_t>(B.foeFloods)); ++i) w.foeFlood.push_back({ fl[i].second, {} });
    }

    bool Phase1(PartyPicture& pic, const Env::View& v, Env::Scratch& s, const ClassProfiles& profiles, const Tuning& t, std::vector<RayAsk>& asks, float sliceMs) {
        using Stage = Work::Stage;
        asks.clear();
        Work& w = pic.work;
        if (w.stage == Stage::Done) return true;
        if (v.stamp != pic.envStamp || v.verts.size() != pic.wallVert.size()) { w = Work{}; pic.counts.viewChanged = 1; return true; }   // every index in the work is the other mesh's
        // The clock only decides where to pause. What is computed never depends on it.
        const auto t0 = std::chrono::steady_clock::now();
        const auto more = [&] { return sliceMs > 0.0f && std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count() < sliceMs; };
        do {
            switch (w.stage) {
            case Stage::Flood:    if (!UnitFlood(pic, v, s, t)) { w.stage = Stage::Generate; w.cursor = 0; } break;
            case Stage::Generate: if (w.cursor < w.gen.size()) UnitGenerate(pic, v, s, t); else w.stage = Stage::Merge; break;
            case Stage::Merge:    UnitMerge(pic); w.stage = Stage::Place; w.cursor = 0; break;
            case Stage::Place:    if (w.cursor < w.hits.size()) UnitPlace(pic, v, t); else w.stage = Stage::Select; break;
            case Stage::Select:   UnitSelect(pic); w.stage = Stage::Offer; w.cursor = 0; break;
            case Stage::Offer:    if (w.cursor < pic.pool.size()) UnitOffer(pic, v, s, t); else w.stage = Stage::Queue; break;
            case Stage::Queue:    UnitQueue(pic); w.stage = Stage::Paths; w.cursor = 0; break;
            case Stage::Paths:    if (w.cursor < w.pathUnits.size()) UnitPath(pic, v, s, t); else { w.stage = Stage::Asks; w.cursor = 0; } break;
            case Stage::Asks:     UnitAsks(pic, profiles, t); asks = w.asked; w.stage = Stage::Done; Spent(w); return true;
            case Stage::Done:     return true;
            }
        } while (more());
        return false;
    }

    void Answer(PartyPicture& pic, std::span<const RayAnswer> answers, const Tuning& t) {
        auto& asked = pic.work.asked; auto& late = pic.late;
        for (const RayAnswer& a : answers) {
            const auto mine = [&](const RayAsk& k) { return k.spot == a.spot && k.viewer == a.viewer; };
            const auto now = std::find_if(asked.begin(), asked.end(), mine);
            const auto old = std::find_if(late.begin(), late.end(), mine);
            if (now == asked.end() && old == late.end()) continue;   // not a question that is open
            if (a.sees == Iaus::Tri::Unknown) continue;              // not an answer: it stays asked and is carried
            const RayAsk q = old != late.end() ? *old : *now;
            if (now != asked.end()) asked.erase(now);
            if (old != late.end()) late.erase(old);
            PairFact f; f.sees = a.sees; f.spotAt = q.p; f.t = q.t; f.actorAt = q.viewerAt; f.actorTri = q.viewerTri;
            if (std::isfinite(a.t) && a.t >= 0.0 && Finite(a.viewerAt)) { f.t = a.t; f.actorAt = a.viewerAt; f.actorTri = kAnyTri; }
            // a kept shooter's spot: the event counts although the runner's fact may already be too far off to keep
            if (a.sees == Iaus::Tri::No && pic.t - f.t <= static_cast<double>(t.spotKeptAgeSec))
                for (Member& m : pic.party) if (m.fixed && m.commit.key == a.spot && m.commit.act.target == a.viewer && Sighted(m.commit.act)) m.marks |= kMarkSightLost;
            if (Stale(pic, { a.spot, a.viewer }, f, (std::max)(pic.t, f.t), t)) continue;
            const auto [it, fresh] = pic.facts.try_emplace({ a.spot, a.viewer }, f);
            if (!fresh && it->second.t <= f.t) it->second = f;
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
            for (std::uint8_t a = 0; a < m.nTop; ++a)
                if (const auto mask = w.Offered(p.spot, p.member, a)) {
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
                if (x->spot >= 0) { spotOf(x->spot); a.kinds = x->kinds; a.kept = x->incumbent; }
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
            m->seen = 0;
        }
    }
}
