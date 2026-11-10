// iaus/Engine.cpp -- the thin engine-facing adapter (contract in iaus/Engine.h). Card I2.
#include "iaus/Engine.h"

#include "Config.h"
#include "Cooldowns.h"
#include "Temperament.h"

#include <unordered_set>

namespace MFO::Iaus::Engine {

    namespace {
        // g_followers domain (#4): the scan's worker only, or ResetAllState with the pump drained.
        std::unordered_map<RE::FormID, Mind> g_minds;
        std::unordered_set<RE::FormID>       g_overCapWarned;
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_budgetWarnedAt;
        std::unordered_map<RE::FormID, std::string> g_lastWalk;   // the last logged fall-past chain (dedupe)

        // Leaf mutex: the worker publishes, the render thread copies. Never held across an engine call.
        std::mutex                              g_readoutMx;
        std::unordered_map<RE::FormID, Readout> g_readouts;

        double Seconds(std::chrono::steady_clock::time_point tp) {
            return std::chrono::duration<double>(tp.time_since_epoch()).count();
        }
    }

    Tick MakeTick(RE::FormID a_actor, std::uint8_t a_rank) {
        Tick t;
        t.tp = std::chrono::steady_clock::now();
        t.in.now = Seconds(t.tp);
        t.in.rapportRank = a_rank;
        t.in.commitWindowSec = static_cast<double>(Config::g_suppressWindow.load()) *
                               (0.88 + 0.24 * static_cast<double>(Temperament(a_actor)));
        // The Temperament hash bits, spread to 64: a stable per-follower stream (design 5.1). The epoch is mixed
        // in by Iaus::Unit, so the seed itself never changes.
        t.in.seed = (static_cast<std::uint64_t>(a_actor) * 0x9E3779B97F4A7C15ull) ^ 0x1A05C0DEull;
        return t;
    }

    void FillCooldowns(RE::FormID a_actor, std::span<Candidate> a_cands, const Tick& a_tick, const Tuning& a_t) {
        for (auto& c : a_cands) {
            const std::uint64_t id = CooldownId(KeyOf(c));
            c.f.hardCooldownLeft = c.f.hardCooldownSec > 0.0f
                ? Cooldowns::Remaining(a_actor, Cooldowns::Kind::IausAction, id, c.f.hardCooldownSec, a_tick.tp)
                : 0.0f;
            // Seconds since it fired, read back from the same store: Remaining over the soft window T is T - since
            // while it is recent, 0 once T has passed (or it never fired) -- both mean "fully recovered".
            const float T = a_t.softCooldownSec;
            const float left = T > 0.0f ? Cooldowns::Remaining(a_actor, Cooldowns::Kind::IausAction, id, T, a_tick.tp)
                                        : 0.0f;
            c.f.sinceFired = left > 0.0f ? T - left : kInf;
        }
    }

    Mind& MindFor(RE::FormID a_actor) { return g_minds[a_actor]; }

    Decision Decide(RE::FormID a_actor, std::span<const Candidate> a_cands, const Profile& a_p, const Tuning& a_t,
                    const Tick& a_tick, double* a_ms) {
        const auto t0 = std::chrono::steady_clock::now();
        Decision d = Iaus::Decide(MindFor(a_actor), a_cands, a_p, a_t, a_tick.in);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (a_ms) *a_ms = ms;

        if (ms > static_cast<double>(a_t.budgetMs)) {
            auto [it, fresh] = g_budgetWarnedAt.try_emplace(a_actor, t1);
            if (fresh || t1 - it->second >= std::chrono::seconds(10)) {
                it->second = t1;
                spdlog::warn("[iaus] {:08X}: decision took {:.3f} ms, over fIausBudgetMs {:.2f} ({} candidates, {} "
                             "evaluated, {} pruned)", a_actor, ms, a_t.budgetMs, d.generated, d.evaluated, d.pruned);
            }
        }
        if (d.overCap && g_overCapWarned.insert(a_actor).second) {
            std::array<int, kFamilies> n{};
            for (const auto& c : a_cands) {
                const auto fi = static_cast<std::size_t>(c.family);
                if (fi < kFamilies) ++n[fi];
            }
            std::string counts;
            for (std::size_t i = 0; i < kFamilies; ++i)
                if (n[i]) counts += std::format(" {}={}", Name(static_cast<Family>(i)), n[i]);
            spdlog::warn("[iaus] {:08X}: {} candidates offered, over iIausMaxCandidates {}: only the first {} are "
                         "scored (once per session).{}", a_actor, d.generated, a_t.maxCandidates, a_t.maxCandidates,
                         counts);
        }
        return d;
    }

    void NoteFired(RE::FormID a_actor, const Candidate& a_c, const Step& a_step, const Tick& a_tick) {
        OnFired(MindFor(a_actor), a_c, a_step, a_tick.in.now);
        Cooldowns::NoteFired(a_actor, Cooldowns::Kind::IausAction, CooldownId(KeyOf(a_c)), a_tick.tp);
    }

    void NotePostureFired(RE::FormID a_actor, const Candidate& a_c, const Tick& a_tick) {
        OnPostureFired(MindFor(a_actor), a_c);
        Cooldowns::NoteFired(a_actor, Cooldowns::Kind::IausAction, CooldownId(KeyOf(a_c)), a_tick.tp);
    }

    void Log(RE::FormID a_actor, std::string_view a_name, const Decision& a_d, std::span<const Candidate> a_cands,
             const Profile& a_p, const Tuning& a_t, const Tick& a_tick, int a_pickedStep,
             std::span<const WalkNote> a_walk, double a_ms, bool a_explain) {
        Mind& m = MindFor(a_actor);
        Key pick{};   // Idle logs under the default key, so an idle stretch is one line per logRepeatSec
        if (!a_d.order.empty()) {
            const std::size_t s = (a_pickedStep >= 0 && static_cast<std::size_t>(a_pickedStep) < a_d.order.size())
                                      ? static_cast<std::size_t>(a_pickedStep) : 0;
            pick = KeyOf(a_cands[a_d.order[s].idx]);
        }
        // A NEW fall-past chain is always logged (a decline is never silent, design 6); a repeat of the same chain
        // waits for the pick's own pacing (on change / every logRepeatSec), like the [eval] chain's dedupe.
        std::string walkSig;
        for (const auto& w : a_walk) walkSig += std::format("{}:{};", w.idx, w.reason);
        auto& lastWalk = g_lastWalk[a_actor];
        const bool newWalk = walkSig != lastWalk;
        if (!ShouldLog(m, pick, a_tick.in.now, a_t) && !newWalk) return;
        lastWalk = std::move(walkSig);
        spdlog::info("[iaus] {} ({:08X}) {}", a_name, a_actor, FormatDecision(a_d, a_cands, a_t, a_pickedStep, a_walk, a_ms));
        if (!a_explain) return;
        std::size_t n = 0;
        for (const auto& r : a_d.ranked) {
            if (n++ >= 3) break;
            spdlog::info("[iaus-why] {} ({:08X}) {}", a_name, a_actor,
                         FormatWhy(a_cands[r.idx], m, a_p, a_t, a_d.rapportRank, a_d.bestHeal));
        }
    }

    void Publish(RE::FormID a_actor, Readout a_r) {
        std::scoped_lock lk(g_readoutMx);
        g_readouts[a_actor] = std::move(a_r);
    }

    bool ReadReadout(RE::FormID a_actor, Readout& a_out) {
        std::scoped_lock lk(g_readoutMx);
        const auto it = g_readouts.find(a_actor);
        if (it == g_readouts.end()) return false;
        a_out = it->second;
        return true;
    }

    void EndDecision(RE::FormID a_actor) {
        if (const auto it = g_minds.find(a_actor); it != g_minds.end()) Iaus::EndDecision(it->second);
    }

    void Forget(RE::FormID a_actor) {
        g_minds.erase(a_actor);
        g_budgetWarnedAt.erase(a_actor);
        g_lastWalk.erase(a_actor);
        std::scoped_lock lk(g_readoutMx);
        g_readouts.erase(a_actor);
    }

    void Clear() {
        g_minds.clear();
        g_overCapWarned.clear();
        g_budgetWarnedAt.clear();
        g_lastWalk.clear();
        std::scoped_lock lk(g_readoutMx);
        g_readouts.clear();
    }

}
