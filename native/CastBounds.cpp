#include "PCH.h"
#include "CastBounds.h"
#include <array>
#include <atomic>
#include <chrono>

// See CastBounds.h for the full rationale. This TU is deliberately tiny and
// dependency-free (only <atomic>/<chrono>) so the combat-thread reader is a
// couple of relaxed atomic loads with no allocation, no lock, no engine call.
namespace MFO::CastBounds {

    namespace {
        // 8 slots = up to 4 concurrent (spell + proxy) executed-cast pairs. A
        // follower runs at most one MFO-executed hand stream at a time, so this
        // comfortably covers a small party; overflow evicts the soonest-to-expire
        // slot (never a crash, never an unbounded write).
        inline constexpr std::size_t kSlots = 8;

        struct Slot {
            std::atomic<std::uint64_t> key{ 0 };       // (actor<<32 | spell); 0 = free
            std::atomic<std::uint64_t> expiryMs{ 0 };  // steady_clock ms; 0 = free
        };
        std::array<Slot, kSlots> g_slots;

        inline std::uint64_t MakeKey(RE::FormID a_actor, RE::FormID a_spell) {
            return (static_cast<std::uint64_t>(a_actor) << 32) | a_spell;
        }
        inline std::uint64_t NowMs() {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        }

        // Arm ONE key. Reuse a slot already holding this key (refresh expiry), else
        // a free/expired slot, else evict the soonest-to-expire. Writer-side; called
        // from worker or main. Publication: store expiry (relaxed) BEFORE key
        // (release) so a combat-thread reader that acquires a matching key is
        // guaranteed to see the paired expiry.
        void ArmKey(std::uint64_t a_key, std::uint64_t a_expiry) {
            const std::uint64_t now = NowMs();
            // 1. an existing slot for this exact key -> refresh.
            for (auto& s : g_slots) {
                if (s.key.load(std::memory_order_acquire) == a_key) {
                    s.expiryMs.store(a_expiry, std::memory_order_relaxed);
                    s.key.store(a_key, std::memory_order_release);
                    return;
                }
            }
            // 2. a free or already-expired slot.
            for (auto& s : g_slots) {
                const std::uint64_t k = s.key.load(std::memory_order_acquire);
                if (k == 0 || s.expiryMs.load(std::memory_order_relaxed) <= now) {
                    s.expiryMs.store(a_expiry, std::memory_order_relaxed);
                    s.key.store(a_key, std::memory_order_release);
                    return;
                }
            }
            // 3. all live -> evict the soonest-to-expire (bounded, never grows).
            Slot* victim = &g_slots[0];
            std::uint64_t soonest = victim->expiryMs.load(std::memory_order_relaxed);
            for (auto& s : g_slots) {
                const std::uint64_t e = s.expiryMs.load(std::memory_order_relaxed);
                if (e < soonest) { soonest = e; victim = &s; }
            }
            victim->expiryMs.store(a_expiry, std::memory_order_relaxed);
            victim->key.store(a_key, std::memory_order_release);
        }
    }

    void Arm(RE::FormID a_actor, RE::FormID a_spell, RE::FormID a_proxy, std::uint32_t a_ttlMs) {
        if (a_actor == 0 || a_spell == 0) return;
        const std::uint64_t expiry = NowMs() + a_ttlMs;
        ArmKey(MakeKey(a_actor, a_spell), expiry);
        if (a_proxy != 0 && a_proxy != a_spell)
            ArmKey(MakeKey(a_actor, a_proxy), expiry);
    }

    void Disarm(RE::FormID a_actor) {
        if (a_actor == 0) return;
        const std::uint64_t hi = static_cast<std::uint64_t>(a_actor) << 32;
        for (auto& s : g_slots) {
            const std::uint64_t k = s.key.load(std::memory_order_acquire);
            if ((k & 0xFFFFFFFF00000000ULL) == hi) {
                s.key.store(0, std::memory_order_release);
                s.expiryMs.store(0, std::memory_order_relaxed);
            }
        }
    }

    bool Live(RE::FormID a_actor, RE::FormID a_spell) {
        if (a_actor == 0 || a_spell == 0) return false;
        const std::uint64_t want = MakeKey(a_actor, a_spell);
        const std::uint64_t now  = NowMs();
        for (auto& s : g_slots) {
            if (s.key.load(std::memory_order_acquire) != want) continue;
            // Matching key seen -> the acquire pairs with ArmKey's release, so the
            // expiry we read is the one published for THIS key.
            if (now < s.expiryMs.load(std::memory_order_relaxed)) return true;
        }
        return false;
    }

    void Reset() {
        for (auto& s : g_slots) {
            s.key.store(0, std::memory_order_release);
            s.expiryMs.store(0, std::memory_order_relaxed);
        }
    }

    std::size_t LiveCount() {
        const std::uint64_t now = NowMs();
        std::size_t n = 0;
        for (auto& s : g_slots)
            if (s.key.load(std::memory_order_acquire) != 0 &&
                now < s.expiryMs.load(std::memory_order_relaxed))
                ++n;
        return n;
    }

}
