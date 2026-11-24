#include "probes/Probes_internal.h"

#include "Config.h"
#include "Followers.h"
#include "State.h"
#include "apmf/APMFBridge.h"
#include "logistics/Logistics.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

// The probe tick: per-follower samples, the drop / jump watch, the lived trust signs, the line budget. See Probes.h.
namespace MFO::Probes {

    namespace {
        // ---- line budget -------------------------------------------------------------------------------------------------------------
        constexpr const char* kKindTag[] = { "probe-trav", "probe-drop", "probe-ray", "probe-trust", "probe-point" };
        struct Budget {
            std::atomic<std::int64_t>  windowStart{ 0 };
            std::atomic<std::int32_t>  used{ 0 };
            std::atomic<std::int32_t>  suppressed{ 0 };
        };
        std::array<Budget, static_cast<std::size_t>(Kind::kCount)> g_budget;

        // ---- tracks (worker only) -----------------------------------------------------------------------------------------------------
        std::unordered_map<RE::FormID, Track> g_tracks;

        // ---- lived signs ---------------------------------------------------------------------------------------------------------------
        struct LivedRow { const char* name; };
        constexpr LivedRow kLivedNames[] = {   // index = Lived
            { "engine-dropped" }, { "leg-blocked-static" }, { "leg-stuck-timeout" }, { "jump" }, { "falling" }, { "traversal-unplanned" },
        };
        static_assert(std::size(kLivedNames) == static_cast<std::size_t>(Lived::kCount), "one name per Lived sign");
        std::mutex g_livedMx;
        std::unordered_map<RE::FormID, std::uint32_t> g_livedCount;
        std::unordered_map<std::uint64_t, std::int64_t> g_livedLast;   // (sign << 32 | fid) -> last line ms

        float Dist2D(const Sample& a, const Sample& b) { return std::hypot(a.x - b.x, a.y - b.y); }

        std::uint32_t CellOf(RE::Actor* a_actor) {
            const auto* c = a_actor->GetParentCell();
            return c ? c->GetFormID() : 0u;
        }

        void Push(Track& a_t, const Sample& a_s) { a_t.ring[a_t.n % kRing] = a_s; ++a_t.n; }

        const char* NameOf(RE::Actor* a) { return (a && a->GetName() && *a->GetName()) ? a->GetName() : "?"; }

        // The drop / fall watch (8.8 "a drop link taken with no graph event"). Reports when he LANDS, so a graph event that closes the move
        // is already in. The navmesh link kind under him needs the N1 snapshot; until then the line carries the heights and the positions.
        void DropWatch(RE::Actor* a_actor, Track& a_t, const Sample& a_prev, const Sample& a_cur) {
            if (!Enabled(Kind::Drop) && !Enabled(Kind::Trust)) return;
            if (!a_t.falling) {
                const Sample* peak = nullptr;
                for (std::size_t i = 0; i < std::min(a_t.n, kRing); ++i) {
                    const Sample& s = a_t.ring[(a_t.n - 1 - i) % kRing];
                    if (a_cur.ms - s.ms > kTuning.dropWindowMs) break;
                    if (!peak || s.z > peak->z) peak = &s;
                }
                if (peak && peak->z - a_cur.z >= kTuning.dropMinZ) { a_t.falling = true; a_t.peak = *peak; }
                return;
            }
            const bool landed = std::fabs(a_cur.z - a_prev.z) < kTuning.landedDz;
            const bool stale  = a_cur.ms - a_t.peak.ms > kTuning.dropWindowMs + 1500;
            if (!landed && !stale) return;
            a_t.falling = false;
            const float dz = a_t.peak.z - a_cur.z;
            if (dz < kTuning.dropMinZ) return;   // he climbed back up: not a drop
            const bool ev = a_t.lastEventMs != 0 && a_t.lastEventMs >= a_t.peak.ms - kTuning.dropEventSlackMs;
            if (Enabled(Kind::Drop) && LineAllowed(Kind::Drop)) {
                std::string player = "?";
                if (const auto* pc = RE::PlayerCharacter::GetSingleton()) {
                    const auto p = pc->GetPosition();
                    player = PosWord(p.x, p.y, p.z);
                }
                spdlog::info("[probe-drop] {:08X} '{}': lost {:.0f} height in {:.1f}s, ran {:.0f} across, from {} to {} | graph traversal event={} | player {} | in flight: {} "
                             "| (navmesh link kind under him: unknown until the N1 snapshot)",
                             a_actor->GetFormID(), a_t.name, dz, (a_cur.ms - a_t.peak.ms) / 1000.0f, Dist2D(a_t.peak, a_cur),
                             PosWord(a_t.peak.x, a_t.peak.y, a_t.peak.z), PosWord(a_cur.x, a_cur.y, a_cur.z),
                             ev ? "YES" : "NONE", player, InFlight(a_actor));
            }
            if (!ev) NoteLived(Lived::Falling, a_actor->GetFormID(), fmt::format("lost {:.0f} height, no graph event", dz).c_str());
        }

        void JumpWatch(RE::Actor* a_actor, Track& a_t, const Sample& a_prev, const Sample& a_cur) {
            const auto dt = a_cur.ms - a_prev.ms;
            if (dt <= 0 || dt > kTuning.jumpMaxDtMs) return;
            const float d = Dist2D(a_prev, a_cur);
            if (d < kTuning.jumpMinDist || d * 1000.0f / static_cast<float>(dt) < kTuning.jumpMinSpeed) return;
            a_t.falling = false;
            NoteLived(Lived::Jump, a_actor->GetFormID(),
                      fmt::format("moved {:.0f} in {} ms ({:.0f} u/s) from {} to {}", d, dt, d * 1000.0f / static_cast<float>(dt),
                                  PosWord(a_prev.x, a_prev.y, a_prev.z), PosWord(a_cur.x, a_cur.y, a_cur.z)).c_str());
        }
    }

    // ---- shared helpers ------------------------------------------------------------------------------------------------------------
    std::int64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool Enabled(Kind a_kind) {
        switch (a_kind) {
        case Kind::Trav:  return Config::g_probeTraversal.load(std::memory_order_relaxed);
        case Kind::Drop:  return Config::g_probeDrop.load(std::memory_order_relaxed);
        case Kind::Ray:   return Config::g_probeRayCost.load(std::memory_order_relaxed);
        case Kind::Trust: return Config::g_probeTrust.load(std::memory_order_relaxed);
        case Kind::Point: return Config::g_probePoint.load(std::memory_order_relaxed);
        default:          return false;
        }
    }

    bool LineAllowed(Kind a_kind) {
        auto&      b   = g_budget[static_cast<std::size_t>(a_kind)];
        const auto now = NowMs();
        const auto ws  = b.windowStart.load(std::memory_order_relaxed);
        if (now - ws >= kTuning.minuteMs) {
            b.windowStart.store(now, std::memory_order_relaxed);
            b.used.store(0, std::memory_order_relaxed);
            if (const int sup = b.suppressed.exchange(0, std::memory_order_relaxed); sup > 0)
                spdlog::info("[{}] {} line(s) suppressed in the last minute by iProbeLinesPerMin", kKindTag[static_cast<std::size_t>(a_kind)], sup);
        }
        if (b.used.fetch_add(1, std::memory_order_relaxed) >= Config::g_probeLinesPerMin.load(std::memory_order_relaxed)) {
            b.suppressed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    const Sample* Newest(const Track& a_t) { return a_t.n ? &a_t.ring[(a_t.n - 1) % kRing] : nullptr; }

    const Sample* AtOrBefore(const Track& a_t, std::int64_t a_ms) {
        const Sample* oldest = nullptr;
        for (std::size_t i = 0; i < std::min(a_t.n, kRing); ++i) {
            const Sample& s = a_t.ring[(a_t.n - 1 - i) % kRing];
            if (s.ms <= a_ms) return &s;
            oldest = &s;
        }
        return oldest;
    }

    Track* TrackOf(RE::FormID a_fid) {
        const auto it = g_tracks.find(a_fid);
        return it == g_tracks.end() ? nullptr : &it->second;
    }

    std::string PosWord(float a_x, float a_y, float a_z) { return fmt::format("({:.0f},{:.0f},{:.0f})", a_x, a_y, a_z); }

    std::string InFlight(RE::Actor* a_actor) {
        if (!a_actor) return "none";
        const RE::FormID id = a_actor->GetFormID();
        std::string out;
        const auto add = [&](bool a_on, const char* a_word) { if (a_on) { if (!out.empty()) out += ','; out += a_word; } };
        add(Logistics::WalkingLootLeg(a_actor), "leg");
        add(APMFBridge::OffenseApproachStands(id), "offense-approach");
        add(APMFBridge::HealApproachStands(id), "heal-approach");
        add(APMFBridge::RangeCloseStands(id), "range-close");
        add(APMFBridge::DisengageStands(id), "disengage");
        add(PointWatching(id), "point");
        add(APMFBridge::IsOwnedCastActive(id) || APMFBridge::IsHealCastActive(id), "cast-claim");
        add(static_cast<bool>(APMFBridge::PinnedTarget(id).get()), "target-pin");
        add(APMFBridge::SneakClaimHeld(id), "sneak-claim");
        add(APMFBridge::PursuitLeashStanding(id), "leash");
        return out.empty() ? "none" : out;
    }

    std::uint32_t LivedCount(RE::FormID a_fid) {
        std::scoped_lock lk(g_livedMx);
        const auto it = g_livedCount.find(a_fid);
        return it == g_livedCount.end() ? 0u : it->second;
    }

    // ---- lived trust signs (8.9) ---------------------------------------------------------------------------------------------------
    void NoteLived(Lived a_sign, RE::FormID a_actor, const char* a_detail) {
        if (!Enabled(Kind::Trust) && !Enabled(Kind::Trav)) return;
        const auto now = NowMs();
        {
            std::scoped_lock lk(g_livedMx);
            ++g_livedCount[a_actor];
            if (!Enabled(Kind::Trust)) return;
            auto& last = g_livedLast[(static_cast<std::uint64_t>(a_sign) << 32) | a_actor];
            if (last != 0 && now - last < kTuning.livedPerActorMs) return;
            last = now;
        }
        if (!LineAllowed(Kind::Trust)) return;
        auto* a = RE::TESForm::LookupByID<RE::Actor>(a_actor);
        std::string where = "?";
        std::uint32_t cell = 0;
        if (a) { const auto p = a->GetPosition(); where = PosWord(p.x, p.y, p.z); cell = CellOf(a); }
        spdlog::info("[probe-trust] {:08X} '{}': sign={} at {} cell {:08X} | {}", a_actor, NameOf(a),
                     kLivedNames[static_cast<std::size_t>(a_sign)].name, where, cell, a_detail ? a_detail : "");
    }

    // ---- the tick ----------------------------------------------------------------------------------------------------------------------
    void Tick() {
        const bool any = Enabled(Kind::Trav) || Enabled(Kind::Drop) || Enabled(Kind::Trust) || Enabled(Kind::Point) || Enabled(Kind::Ray);
        if (!any) return;
        const auto now = NowMs();
        std::unordered_map<RE::FormID, bool> seen;
        for (const auto& h : Followers::g_active) {
            auto* actor = h.get().get();
            if (!actor) continue;
            const auto id  = actor->GetFormID();
            const auto rec = g_followers.find(id);
            if (rec == g_followers.end() || !rec->second.mfoEnabled) continue;
            seen[id] = true;
            if (Enabled(Kind::Trav)) TraversalService(actor);
            if (actor->IsDead() || !actor->Is3DLoaded()) continue;
            auto& t = g_tracks[id];
            if (t.name.empty()) t.name = NameOf(actor);
            const Sample* prev = Newest(t);
            if (prev && now - prev->ms < kTuning.sampleMs) continue;
            const auto p = actor->GetPosition();
            Sample cur{ now, p.x, p.y, p.z, CellOf(actor) };
            if (prev && prev->cell != cur.cell) { t.n = 0; t.falling = false; prev = nullptr; }   // a cell change is not a move
            if (prev) {
                const Sample before = *prev;
                Push(t, cur);
                JumpWatch(actor, t, before, cur);
                DropWatch(actor, t, before, cur);
            } else {
                Push(t, cur);
            }
            if (PointWatching(id)) PointSample(actor, cur);
        }
        for (auto it = g_tracks.begin(); it != g_tracks.end();) it = seen.contains(it->first) ? std::next(it) : g_tracks.erase(it);   // dismissed
        TraversalDrain(now);
        RayTick(now);
    }

    void Clear() {
        g_tracks.clear();
        {
            std::scoped_lock lk(g_livedMx);
            g_livedCount.clear();
            g_livedLast.clear();
        }
        TraversalClear();
        PointClear();
        RayClear();
    }

}
