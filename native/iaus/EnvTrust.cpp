// iaus/EnvTrust.cpp -- marks, levels and what clears them (contract in iaus/EnvTrust.h). PURE.
#include "iaus/EnvTrust.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace MFO::Iaus::Trust {

    namespace {
        float Dist(const Env::Vec3& a, const Env::Vec3& b) { const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z; return std::sqrt(x * x + y * y + z * z); }
        bool Finite(const Env::Vec3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
        const SignRow& RowOf(Sign s) { return kTrustSigns[static_cast<std::size_t>(s)]; }
        // Does a -> b cross the line a_ahead past the place the mark was seen at, the way it was going, within a_u of it, and (a_cos > 0) along it?
        bool Crosses(const Mark& m, const Env::Vec3& a, const Env::Vec3& b, float ahead, float u, float cosMin, const Rules& r) {
            if (m.dx == 0.0f && m.dy == 0.0f) return Dist(a, m.at) <= u || Dist(b, m.at) <= u;   // it was going nowhere: being there is enough
            const float gx = m.at.x + m.dx * ahead, gy = m.at.y + m.dy * ahead;
            const float sa = (a.x - gx) * m.dx + (a.y - gy) * m.dy, sb = (b.x - gx) * m.dx + (b.y - gy) * m.dy;
            if (!(sa < 0.0f && sb >= 0.0f)) return false;
            if (cosMin > 0.0f) { const float lx = b.x - a.x, ly = b.y - a.y, f = sb - sa; if (!(f > 0.0f && f * f >= cosMin * cosMin * (lx * lx + ly * ly))) return false; }
            const float k = sa / (sa - sb), x = a.x + (b.x - a.x) * k, y = a.y + (b.y - a.y) * k, z = a.z + (b.z - a.z) * k;
            return (x - gx) * (x - gx) + (y - gy) * (y - gy) <= u * u && std::fabs(z - m.at.z) <= r.levelU;
        }
        void Say(Table& t, Region region, const Mark& why) {
            const Level l = Of(t, region);
            if (l == Level::Trusted) return;
            const std::pair<Region, Level> k{ region, l };
            if (std::find(t.said.begin(), t.said.end(), k) != t.said.end()) return;
            t.said.push_back(k); t.log.push_back({ region, l, why });
        }
    }

    const char* Name(Level l) { return l == Level::Trusted ? "trusted" : l == Level::Doubted ? "doubted" : "distrusted"; }

    Region RegionOf(const Env::Vec3& p, const Rules& r) {
        if (!Finite(p)) return 0;
        const auto c = [](float v, float u) { return static_cast<std::int64_t>(std::floor((std::clamp)(v, -4.0e7f, 4.0e7f) / u)) + (std::int64_t{ 1 } << 20); };
        return (c(p.x, r.regionU) << 42) | (c(p.y, r.regionU) << 21) | c(p.z, r.regionZ);
    }

    Level Of(const Table& t, Region region) {
        std::vector<const Mark*> heads;   // the first mark of each lived event: an event is as long as its head says, never a chain
        std::array<int, kSigns> n{}; int kinds = 0; Level out = Level::Trusted;
        for (std::size_t i = 0; i < t.marks.size(); ++i) {
            const Mark& m = t.marks[i];
            if (m.region != region || m.sign >= Sign::kCount) continue;
            const SignRow& row = RowOf(m.sign); int& c = n[static_cast<std::size_t>(m.sign)];
            if (row.lived) {
                if (std::any_of(heads.begin(), heads.end(), [&](const Mark* o) {   // the same event as an older one of this region
                        return (m.leg != 0 && o->leg == m.leg && o->who == m.who) || std::fabs(m.t - o->t) < static_cast<double>(t.rules.eventSec); })) continue;
                heads.push_back(&m);
            }
            if (c++ == 0) ++kinds;
            out = (std::max)(out, row.alone);
            if ((row.lived && c >= t.rules.repeat) || kinds >= 2) out = Level::Distrusted;
        }
        return out;
    }
    Level Of(const Table& t, const Env::Vec3& p) {
        if (t.marks.empty()) return Level::Trusted;
        for (const Trail& q : t.trail) if (Dist(q.p, p) <= t.rules.trailU) return Level::Trusted;
        return Of(t, RegionOf(p, t.rules));
    }
    Level RouteLevel(const Table& t, const Env::Vec3& from, std::span<const Env::Vec3> via) {
        if (t.marks.empty()) return Level::Trusted;
        const Region own = RegionOf(from, t.rules);
        Level worst = Level::Trusted; bool out = false; Env::Vec3 prev = from;
        for (const Env::Vec3& p : via) {
            const Region r = RegionOf(p, t.rules);
            out = out || r != own;
            if (out) worst = (std::max)(worst, Of(t, r));
            if (worst == Level::Distrusted) return worst;
            for (const Mark& m : t.marks) if (Crosses(m, prev, p, 0.5f * t.rules.aheadU, t.rules.routeU, 0.0f, t.rules)) worst = (std::max)(worst, Of(t, m.region));
            prev = p;
        }
        return worst;
    }
    bool Refused(const Table& t, FormID who, std::uint64_t spot) {
        return std::any_of(t.refused.begin(), t.refused.end(), [&](const Refusal& r) { return r.who == who && r.spot == spot; });
    }

    bool Note(Table& t, Sign s, FormID who, const Env::Vec3& at, const Env::Vec3& toward, double now, float a, float b, std::uint32_t leg) {
        if (s >= Sign::kCount || !Finite(at)) return false;
        Mark m; m.sign = s; m.who = who; m.t = now; m.at = at; m.a = a; m.b = b; m.leg = leg;
        if (Finite(toward)) {
            const float x = toward.x - at.x, y = toward.y - at.y, l = std::sqrt(x * x + y * y);
            if (l > 1.0f) { m.dx = x / l; m.dy = y / l; }
        }
        m.region = RegionOf({ at.x + m.dx * t.rules.aheadU, at.y + m.dy * t.rules.aheadU, at.z }, t.rules);
        for (const Mark& o : t.marks) if (o.region == m.region && o.sign == s && o.who == who && now - o.t >= 0.0 && now - o.t < static_cast<double>(t.rules.repeatSec)) return false;
        if (t.marks.size() >= static_cast<std::size_t>((std::max)(1, t.rules.marks))) t.marks.erase(t.marks.begin());
        t.marks.push_back(m); ++t.gen;
        Say(t, m.region, m);
        return true;
    }

    int Leg(Table& t, const LegEvent& e) {
        int made = 0;
        if (e.end == End::Blocked || e.end == End::Refused) {   // a blocked leg remembers its spot (S2 review F6): else the next window files the same one
            std::erase_if(t.refused, [&](const Refusal& r) { return r.who == e.who && r.spot == e.spot; });
            if (t.refused.size() >= static_cast<std::size_t>((std::max)(1, t.rules.refused))) t.refused.erase(t.refused.begin());
            t.refused.push_back({ e.who, e.spot, e.t });
        }
        if (e.laneClear && e.end == End::Stuck && Note(t, Sign::NoGround, e.who, e.at, e.toward, e.t, e.planned, e.walked, e.leg)) ++made;
        if (e.laneClear && e.end == End::Refused && Note(t, Sign::EngineRefused, e.who, e.at, e.toward, e.t, e.planned, e.walked, e.leg)) ++made;
        if ((e.end == End::Arrived || e.end == End::Walking) && e.planned > 0.0f && Seen(Sign::LengthFar, e.walked / e.planned) && e.walked - e.planned >= t.rules.lengthFloorU) {
            // he got there another way: every part of our route he did not come by is in question, one mark a region
            std::vector<Region> done;
            for (std::size_t i = 0; i < e.route.size() && i < 32; ++i) {
                if (e.passed & (1u << i)) continue;
                const Env::Vec3& to = e.route[i]; const Env::Vec3 from = i ? e.route[i - 1] : e.at;
                const Region r = RegionOf(to, t.rules);
                if (std::find(done.begin(), done.end(), r) != done.end()) continue;
                done.push_back(r);
                if (Note(t, Sign::LengthFar, e.who, { to.x, to.y, to.z }, i + 1 < e.route.size() ? e.route[i + 1] : Env::Vec3{ 2.0f * to.x - from.x, 2.0f * to.y - from.y, to.z }, e.t, e.planned, e.walked, e.leg)) ++made;
            }
        }
        return made;
    }

    void Walked(Table& t, bool player, const Env::Vec3& a, const Env::Vec3& b, double now) {
        if (!Finite(a) || !Finite(b)) return;
        if (player) {
            if (t.trail.empty() || Dist(t.trail.back().p, b) >= t.rules.trailStepU) {
                if (t.trail.size() >= static_cast<std::size_t>((std::max)(1, t.rules.trail))) t.trail.erase(t.trail.begin());
                t.trail.push_back({ b, now });
            } else t.trail.back().t = now;
        }
        if (t.marks.empty()) return;
        if (std::erase_if(t.marks, [&](const Mark& m) { return Crosses(m, a, b, t.rules.clearAheadU, t.rules.clearU, t.rules.clearCos, t.rules); })) ++t.gen;   // the marks it crosses, no other
    }

    void Forget(Table& t) {
        if (!t.marks.empty()) ++t.gen;
        t.marks.clear(); t.refused.clear();
    }
    void Decay(Table& t, double now) {
        const auto n = t.marks.size();
        std::erase_if(t.marks, [&](const Mark& m) { return m.sign >= Sign::kCount || !(now - m.t >= 0.0 && now - m.t < static_cast<double>(RowOf(m.sign).decaySec)); });
        if (t.marks.size() != n) ++t.gen;
        std::erase_if(t.refused, [&](const Refusal& r) { return !(now - r.t >= 0.0 && now - r.t < static_cast<double>(t.rules.refusedSec)); });
        std::erase_if(t.trail, [&](const Trail& q) { return !(now - q.t >= 0.0 && now - q.t < static_cast<double>(t.rules.trailSec)); });
    }

    std::string Text(const Line& l, const char* cell) {
        const std::int64_t m = (std::int64_t{ 1 } << 21) - 1, o = std::int64_t{ 1 } << 20;
        char b[320];
        std::snprintf(b, sizeof b, "[env-trust] %s region %lld,%lld,%lld %s: %s (body %08x at %.0f %.0f %.0f going %.2f %.2f, planned %.0f, walked %.0f)", cell ? cell : "?",
            static_cast<long long>(((l.region >> 42) & m) - o), static_cast<long long>(((l.region >> 21) & m) - o), static_cast<long long>((l.region & m) - o), Name(l.level),
            l.why.sign < Sign::kCount ? RowOf(l.why.sign).name : "?", l.why.who, static_cast<double>(l.why.at.x), static_cast<double>(l.why.at.y), static_cast<double>(l.why.at.z),
            static_cast<double>(l.why.dx), static_cast<double>(l.why.dy), static_cast<double>(l.why.a), static_cast<double>(l.why.b));
        return b;
    }
}
