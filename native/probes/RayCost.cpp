#include "probes/Probes_internal.h"

#include <array>
#include <atomic>
#include <chrono>

#include <spdlog/spdlog.h>

// [probe-ray]: what ONE engine ray costs, timed exactly where Sightline already casts it, on the thread that already does it (the main
// thread). 8.7 budgets the movement pass's ray batch at "about 5 to 20 microseconds each (ESTIMATE, to be measured by a probe before the
// budget is trusted)": this is that probe. The RayTimer only adds to atomic counters (no log, no lock, no allocation on the main thread);
// the worker prints one summary per kRaySummaryMs. No new ray is ever cast by this file.
namespace MFO::Probes {

    namespace {
        constexpr std::size_t kSites = static_cast<std::size_t>(RaySite::kCount);
        constexpr std::size_t kBuckets = 16;   // bucket b = [2^b, 2^(b+1)) microseconds, bucket 0 also holds under 1 us, the last is the open end
        constexpr const char* kSiteName[kSites] = { "own-ray(bhkWorld)", "point-ray(bhkWorld)", "engine-los(HasLineOfSight)" };

        struct Acc {
            std::atomic<std::uint64_t> n{ 0 };
            std::atomic<std::uint64_t> totalNs{ 0 };
            std::atomic<std::uint64_t> maxNs{ 0 };
            std::array<std::atomic<std::uint32_t>, kBuckets> hist{};
        };
        std::array<Acc, kSites> g_acc;
        std::int64_t g_lastSummaryMs = 0;   // worker

        std::int64_t SteadyNs() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    }

    void NoteRay(RaySite a_site, std::int64_t a_ns) {
        if (a_ns < 0) return;
        auto& a = g_acc[static_cast<std::size_t>(a_site)];
        a.n.fetch_add(1, std::memory_order_relaxed);
        a.totalNs.fetch_add(static_cast<std::uint64_t>(a_ns), std::memory_order_relaxed);
        auto cur = a.maxNs.load(std::memory_order_relaxed);
        while (static_cast<std::uint64_t>(a_ns) > cur && !a.maxNs.compare_exchange_weak(cur, static_cast<std::uint64_t>(a_ns), std::memory_order_relaxed)) {}
        std::uint64_t us = static_cast<std::uint64_t>(a_ns) / 1000u;
        std::size_t b = 0;
        while (us > 1 && b + 1 < kBuckets) { us >>= 1; ++b; }
        a.hist[b].fetch_add(1, std::memory_order_relaxed);
    }

    RayTimer::RayTimer(RaySite a_site) noexcept : m_site(a_site), m_t0(0) {
        if (Enabled(Kind::Ray)) m_t0 = SteadyNs();
    }
    RayTimer::~RayTimer() {
        if (m_t0 != 0) NoteRay(m_site, SteadyNs() - m_t0);
    }

    void RayTick(std::int64_t a_nowMs) {
        if (!Enabled(Kind::Ray)) return;
        if (g_lastSummaryMs == 0) { g_lastSummaryMs = a_nowMs; return; }
        if (a_nowMs - g_lastSummaryMs < kTuning.raySummaryMs) return;
        const float secs = (a_nowMs - g_lastSummaryMs) / 1000.0f;
        g_lastSummaryMs = a_nowMs;
        for (std::size_t s = 0; s < kSites; ++s) {
            auto& a = g_acc[s];
            const auto n = a.n.exchange(0, std::memory_order_relaxed);
            const auto total = a.totalNs.exchange(0, std::memory_order_relaxed);
            const auto mx = a.maxNs.exchange(0, std::memory_order_relaxed);
            std::array<std::uint32_t, kBuckets> h{};
            for (std::size_t b = 0; b < kBuckets; ++b) h[b] = a.hist[b].exchange(0, std::memory_order_relaxed);
            if (n == 0 || !LineAllowed(Kind::Ray)) continue;
            // p50 / p95 as the UPPER edge (microseconds) of the bucket holding that rank: a coarse, honest bound.
            const auto edge = [&](double a_q) {
                const double want = a_q * static_cast<double>(n);
                double acc = 0;
                for (std::size_t b = 0; b < kBuckets; ++b) {
                    acc += h[b];
                    if (acc >= want) return static_cast<double>(1u << (b + 1));
                }
                return static_cast<double>(1u << kBuckets);
            };
            spdlog::info("[probe-ray] {}: {} rays in {:.0f}s ({:.1f}/s) | mean {:.1f} us, p50 <= {:.0f} us, p95 <= {:.0f} us, max {:.1f} us | main thread, rays Sightline already casts",
                         kSiteName[s], n, secs, n / secs, total / 1000.0 / static_cast<double>(n), edge(0.5), edge(0.95), mx / 1000.0);
        }
    }

    void RayClear() {
        for (auto& a : g_acc) {
            a.n.store(0); a.totalNs.store(0); a.maxNs.store(0);
            for (auto& b : a.hist) b.store(0);
        }
        g_lastSummaryMs = 0;
    }

}
