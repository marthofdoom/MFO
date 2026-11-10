// Cooldowns.cpp -- the runtime last-fired store (contract in Cooldowns.h). Card G9.
#include "Cooldowns.h"

#include <cmath>

namespace MFO::Cooldowns {

    namespace {
        struct Key {
            RE::FormID    actor = 0;
            Kind          kind = Kind::Rule;
            std::uint64_t id = 0;
            bool operator==(const Key&) const = default;
        };
        struct KeyHash {
            std::size_t operator()(const Key& k) const noexcept {
                std::size_t h = std::hash<std::uint64_t>{}(k.id);
                h ^= std::hash<std::uint32_t>{}(k.actor) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                h ^= static_cast<std::size_t>(k.kind) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                return h;
            }
        };

        std::mutex g_mx;
        std::unordered_map<Key, std::chrono::steady_clock::time_point, KeyHash> g_lastFired;

        // Housekeeping bound: an entry older than this can no longer cool anything (the row codec clamps a
        // cooldown to 600 s, Serialization.h), so it is dropped when the map grows. A deleted row's entry, or a
        // dismissed follower's, goes the same way. Never an expiry on a LIVE cooldown (principle 9).
        constexpr auto kStale = std::chrono::seconds(601);
        constexpr std::size_t kSweepAt = 512;
    }

    void NoteFired(RE::FormID a_actor, Kind a_kind, std::uint64_t a_id,
                   std::chrono::steady_clock::time_point a_now) {
        std::scoped_lock lk(g_mx);
        if (g_lastFired.size() >= kSweepAt)
            std::erase_if(g_lastFired, [&](const auto& kv) { return a_now - kv.second > kStale; });
        g_lastFired[Key{ a_actor, a_kind, a_id }] = a_now;
    }

    float Remaining(RE::FormID a_actor, Kind a_kind, std::uint64_t a_id, float a_cooldownSec,
                    std::chrono::steady_clock::time_point a_now) {
        if (!(a_cooldownSec > 0.0f)) return 0.0f;   // also refuses NaN
        std::scoped_lock lk(g_mx);
        const auto it = g_lastFired.find(Key{ a_actor, a_kind, a_id });
        if (it == g_lastFired.end()) return 0.0f;
        const float since = std::chrono::duration<float>(a_now - it->second).count();
        const float left = a_cooldownSec - since;
        return left > 0.0f ? left : 0.0f;
    }

    void ClearAll() {
        std::scoped_lock lk(g_mx);
        g_lastFired.clear();
    }

    std::string CoolingReason(float a_remainingSec) {
        const int s = (std::max)(1, static_cast<int>(std::ceil(a_remainingSec)));
        return std::format("{}{}s)", kCoolingPrefix, s);
    }

}
