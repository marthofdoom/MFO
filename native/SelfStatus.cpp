#include "PCH.h"
#include "SelfStatus.h"
#include "MainThread.h"
#include "cast/Actuation.h"   // LiveCommandedActors (card G5): the main-thread commanded-actor read
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace MFO::SelfStatus {

    namespace {

        using Clock = std::chrono::steady_clock;

        // Sizes from the real cadence (principle 9): the Evaluator asks once per follower per service lap
        // (133 ms x party, ~1.3 s for ten) and a poison lasts seconds to a minute. Refresh when the mirror is a
        // second old; trust it for five (more than three missed refreshes); a lost refresh (a load drops the
        // pump's queue, MainThread::Clear) is re-asked after kAskRetrySec, so it can never wedge.
        constexpr double kRefreshSec  = 1.0;
        constexpr double kTrustSec    = 5.0;
        constexpr double kAskRetrySec = 3.0;

        struct Entry {
            bool                poisoned = false;
            std::vector<RE::FormID> commanded;   // card G5: the live actors he commands (Actuation::LiveCommandedActors)
            Clock::time_point   stamp{};   // when the last MAIN-thread read finished (epoch = never)
            Clock::time_point   asked{};   // when the refresh in flight was posted (epoch = none in flight)
        };
        std::mutex                                     g_mx;
        std::unordered_map<RE::FormID, Entry>          g_cache;

        double Age(const Clock::time_point& a_then, const Clock::time_point& a_now) {
            return std::chrono::duration<double>(a_now - a_then).count();
        }

        // MAIN THREAD ONLY: the live effect list (it mutates under the magic caster on this thread).
        bool ReadPoisoned(RE::Actor* a_actor) {
            auto* mt   = a_actor ? a_actor->AsMagicTarget() : nullptr;
            auto* list = mt ? mt->GetActiveEffectList() : nullptr;
            if (!list) return false;
            for (auto* ae : *list) {
                if (!ae || !ae->spell) continue;
                if (ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) continue;
                if (ae->spell->IsPoison()) return true;   // NOT GetSpellType(): AlchemyItem returns kAlchemy for it (vfunc 0x53)
            }
            return false;
        }

        void PostRefresh(RE::FormID a_fid) {
            MainThread::Post([a_fid]() {
                bool poisoned = false;
                std::vector<RE::FormID> commanded;
                // A LOADED actor only: an unloaded one has no readable list, so it is recorded NOT poisoned (the
                // fail-closed answer), never a claim that he was cured. The same for his commanded actors (card G5):
                // an unloaded follower is recorded commanding none.
                if (auto* f = RE::TESForm::LookupByID<RE::Actor>(a_fid); f && f->Is3DLoaded()) {
                    poisoned  = ReadPoisoned(f);
                    commanded = Actuation::LiveCommandedActors(f);
                }
                std::scoped_lock lk(g_mx);
                auto& e    = g_cache[a_fid];
                e.poisoned = poisoned;
                e.commanded = std::move(commanded);
                e.stamp    = Clock::now();
                e.asked    = Clock::time_point{};
            });
        }

    }

    void Clear() {
        std::scoped_lock lk(g_mx);
        g_cache.clear();
    }

    namespace {
        // The shared worker-side read: hands the entry to a_read (UNDER g_mx, so a_read only reads, copies nothing it
        // does not need, and calls nothing) when it is FRESH (<= kTrustSec), and posts ONE refresh when it is stale and
        // none is in flight. Returns false (a_read not called) with no fresh measurement.
        template <class F>
        bool Lookup(RE::Actor* a_actor, F&& a_read) {
            if (!a_actor || !MainThread::IsInstalled()) return false;   // VR: no pump, no read (see the header)
            const RE::FormID fid = a_actor->GetFormID();
            const auto       now = Clock::now();
            bool fresh = false;
            bool ask   = false;
            {
                std::scoped_lock lk(g_mx);
                auto& e = g_cache[fid];
                const bool measured = e.stamp.time_since_epoch().count() != 0;
                if (measured && Age(e.stamp, now) <= kTrustSec) { a_read(e); fresh = true; }
                const bool stale    = !measured || Age(e.stamp, now) >= kRefreshSec;
                const bool inFlight = e.asked.time_since_epoch().count() != 0 && Age(e.asked, now) < kAskRetrySec;
                if (stale && !inFlight) { e.asked = now; ask = true; }
            }
            if (ask) PostRefresh(fid);   // outside the mutex: Post takes its own queue lock
            return fresh;
        }
    }

    bool Poisoned(RE::Actor* a_actor) {
        bool v = false;
        return Lookup(a_actor, [&](const Entry& e) { v = e.poisoned; }) && v;
    }

    bool HasSummon(RE::Actor* a_actor) {
        std::size_t n = 0;
        return Lookup(a_actor, [&](const Entry& e) { n = e.commanded.size(); }) && n > 0;
    }

    bool NoSummon(RE::Actor* a_actor) {
        std::size_t n = 0;
        return Lookup(a_actor, [&](const Entry& e) { n = e.commanded.size(); }) && n == 0;   // a stale read is NOT "no summon" (fail closed)
    }

    std::vector<RE::FormID> CommandedLive(RE::Actor* a_actor) {
        std::vector<RE::FormID> out;
        Lookup(a_actor, [&](const Entry& e) { out = e.commanded; });
        return out;
    }

}
