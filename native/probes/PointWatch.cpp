#include "probes/Probes_internal.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

// [probe-point]: the ch.24 POINT road watched as a general mover (iaus-sim-v4.md 8.3). The question: when a follower holds a point, does a
// small radius pin him, and what does he do inside it (stand, circle, drift out)? MFO files point claims in two places today, give ground
// (apmf/GroundApproach.cpp, R 150) and the disengage run (apmf/Disengage.cpp, R declared by the caller). Those two call PointOpen /
// PointClose; this file samples his position while the claim stands (Probes.cpp's tick, no extra read) and prints the numbers at the end.
// PASSIVE: it files no claim of its own. A radius smaller than the two callers use is therefore NOT probed here: that needs a claim with a
// small R, which is an engine request, so it belongs to a Harbinger / H1 brief (see the report).
namespace MFO::Probes {

    namespace {
        struct Watch {
            std::string   kind;
            RE::NiPoint3  pt{};
            float         radius = 0;
            std::int64_t  openMs = 0, lastLineMs = 0;
            Sample        prev{};
            bool          havePrev = false, prevInside = false;
            std::int64_t  firstInsideMs = 0;
            float         insideSec = 0, pathInside = 0, minDist = 1e9f, maxInside = 0, startDist = 0, lastDist = 0, startZ = 0;
            int           samples = 0;
        };
        std::mutex g_mx;
        std::unordered_map<RE::FormID, Watch> g_watch;

        float Dist3(const Sample& s, const RE::NiPoint3& p) { return std::sqrt((s.x - p.x) * (s.x - p.x) + (s.y - p.y) * (s.y - p.y) + (s.z - p.z) * (s.z - p.z)); }
    }

    void PointOpen(RE::FormID a_actor, const char* a_kind, const RE::NiPoint3& a_point, float a_radius) {
        if (!Enabled(Kind::Point)) return;
        Watch w;
        w.kind = a_kind ? a_kind : "?";
        w.pt = a_point;
        w.radius = a_radius;
        w.openMs = NowMs();
        {
            std::scoped_lock lk(g_mx);
            g_watch[a_actor] = w;
        }
        if (LineAllowed(Kind::Point))
            spdlog::info("[probe-point] {:08X}: {} claim OPEN at {} R {:.0f}", a_actor, w.kind, PosWord(a_point.x, a_point.y, a_point.z), a_radius);
    }

    bool PointWatching(RE::FormID a_fid) {
        std::scoped_lock lk(g_mx);
        return g_watch.contains(a_fid);
    }

    void PointSample(RE::Actor* a_actor, const Sample& a_cur) {
        const RE::FormID id = a_actor->GetFormID();
        std::string line;   // built under the lock, completed (InFlight takes this lock too) and logged after it
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_watch.find(id);
            if (it == g_watch.end()) return;
            Watch& w = it->second;
            const float d = Dist3(a_cur, w.pt);
            if (!w.havePrev) { w.startDist = d; w.startZ = a_cur.z; }
            const bool inside = d <= w.radius;
            if (w.havePrev) {
                const float dt = (a_cur.ms - w.prev.ms) / 1000.0f;
                if (inside && w.prevInside && dt > 0 && dt < 2.0f) {
                    w.insideSec += dt;
                    w.pathInside += std::hypot(a_cur.x - w.prev.x, a_cur.y - w.prev.y);
                }
            }
            if (inside && !w.firstInsideMs) w.firstInsideMs = a_cur.ms;
            if (inside) w.maxInside = (std::max)(w.maxInside, d);
            w.minDist = (std::min)(w.minDist, d);
            w.lastDist = d;
            w.prev = a_cur;
            w.prevInside = inside;
            w.havePrev = true;
            ++w.samples;
            if (a_cur.ms - w.lastLineMs >= kTuning.pointLineMs) {
                w.lastLineMs = a_cur.ms;
                line = fmt::format("[probe-point] {:08X}: {} {:.1f}s: {:.0f} from the point (R {:.0f}, {}), dz {:+.0f}, walked {:.0f} inside over {:.1f}s",
                                   id, w.kind, (a_cur.ms - w.openMs) / 1000.0f, d, w.radius, inside ? "INSIDE" : "outside", a_cur.z - w.pt.z,
                                   w.pathInside, w.insideSec);
            }
        }
        if (!line.empty() && LineAllowed(Kind::Point)) spdlog::info("{} | in flight: {}", line, InFlight(a_actor));
    }

    void PointClose(RE::FormID a_actor, const char* a_why) {
        Watch w;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_watch.find(a_actor);
            if (it == g_watch.end()) return;
            w = it->second;
            g_watch.erase(it);
        }
        if (!LineAllowed(Kind::Point)) return;
        const float total = (NowMs() - w.openMs) / 1000.0f;
        const float arrive = w.firstInsideMs ? (w.firstInsideMs - w.openMs) / 1000.0f : -1.0f;
        spdlog::info("[probe-point] {:08X}: {} claim CLOSED ({}) after {:.1f}s, R {:.0f}: start dist {:.0f}, closest {:.0f}, last {:.0f}, first inside after {:.1f}s, "
                     "inside {:.1f}s walking {:.0f} units ({:.0f} u/s, farthest {:.0f} from the point), {} samples",
                     a_actor, w.kind, a_why ? a_why : "", total, w.radius, w.startDist, w.minDist < 1e8f ? w.minDist : -1.0f, w.lastDist, arrive, w.insideSec,
                     w.pathInside, w.insideSec > 0 ? w.pathInside / w.insideSec : 0.0f, w.maxInside, w.samples);
    }

    void PointClear() {
        std::scoped_lock lk(g_mx);
        g_watch.clear();
    }

}
