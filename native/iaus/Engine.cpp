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
        std::unordered_set<RE::FormID>       g_badFactWarned;
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_budgetWarnedAt;
        std::unordered_map<RE::FormID, std::string> g_lastWalk;   // the last logged fall-past chain (dedupe)
        std::unordered_map<RE::FormID, bool> g_source;            // card I6: true = IAUS served his last lap
        std::unordered_map<std::uint64_t, double> g_paced;        // card I6: (actor, channel) -> last logged (seconds)
        // The decision epoch never goes back (review round 1, backlog item 1, fixed here): a Mind created after
        // Forget / Clear starts above every epoch handed out so far this process, so a surviving rank-carrying lock
        // (cast lock, heal claim) can never outrank a new decision by an epoch restarting at 0.
        std::uint32_t g_epochFloor = 0;
        void Retire(const Mind& m) { g_epochFloor = (std::max)(g_epochFloor, m.epoch); }

        // Leaf mutex: the worker publishes, the render thread copies. Never held across an engine call.
        std::mutex                              g_readoutMx;
        struct StampedReadout { Readout r; std::chrono::steady_clock::time_point at; };
        std::unordered_map<RE::FormID, StampedReadout> g_readouts;

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

    namespace {
        static_assert(Cooldowns::kFollowerPowerAttack == 1 && Cooldowns::kFollowerTaunt == 2, "iaus/Core.h SharedCooldownId mirrors these");

        // The Cooldowns identity of a candidate: power attack and taunt are ONE cooldown per follower (Kind::Follower,
        // shared with the gambit rows, marth Q69 / Q73); everything else keeps its (family, posture, form) IausAction id.
        struct CdId { Cooldowns::Kind kind; std::uint64_t id; };
        CdId CdOf(const Candidate& a_c) {
            if (const std::uint64_t shared = SharedCooldownId(a_c.family); shared != 0)
                return { Cooldowns::Kind::Follower, shared };
            return { Cooldowns::Kind::IausAction, CooldownId(KeyOf(a_c)) };
        }
    }

    void FillCooldowns(RE::FormID a_actor, std::span<Candidate> a_cands, const Tick& a_tick, const Tuning& a_t) {
        for (auto& c : a_cands) {
            const CdId cd = CdOf(c);
            c.f.hardCooldownLeft = c.f.hardCooldownSec > 0.0f
                ? Cooldowns::Remaining(a_actor, cd.kind, cd.id, c.f.hardCooldownSec, a_tick.tp)
                : 0.0f;
            // Seconds since it fired, read back from the same store: Remaining over the soft window T is T - since
            // while it is recent, 0 once T has passed (or it never fired) -- both mean "fully recovered".
            const float T = a_t.softCooldownSec;
            const float left = T > 0.0f ? Cooldowns::Remaining(a_actor, cd.kind, cd.id, T, a_tick.tp)
                                        : 0.0f;
            c.f.sinceFired = left > 0.0f ? T - left : kInf;
        }
    }

    Mind& MindFor(RE::FormID a_actor) {
        auto [it, fresh] = g_minds.try_emplace(a_actor);
        if (fresh) it->second.epoch = g_epochFloor;
        return it->second;
    }

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
        if (d.badFacts && g_badFactWarned.insert(a_actor).second) {
            const auto& c = a_cands[static_cast<std::size_t>(d.firstBadFact)];
            spdlog::warn("[iaus] {:08X}: {} candidate(s) VETOED for a NaN/inf fact, first: {} -- a fact reader is "
                         "broken (once per follower per session; every [iaus] line counts them as BAD-FACTS)",
                         a_actor, d.badFacts, Label(c));
        }
        return d;
    }

    void NoteFired(RE::FormID a_actor, const Candidate& a_c, const Step& a_step, const Tick& a_tick,
                   bool a_startCooldown) {
        OnFired(MindFor(a_actor), a_c, a_step, a_tick.in.now);
        if (a_startCooldown)
            Cooldowns::NoteFired(a_actor, CdOf(a_c).kind, CdOf(a_c).id, a_tick.tp);
    }

    void NotePostureFired(RE::FormID a_actor, const Candidate& a_c, const Tick& a_tick) {
        OnPostureFired(MindFor(a_actor), a_c);
        Cooldowns::NoteFired(a_actor, CdOf(a_c).kind, CdOf(a_c).id, a_tick.tp);
    }

    void Log(RE::FormID a_actor, std::string_view a_name, const Decision& a_d, std::span<const Candidate> a_cands,
             const Profile& a_p, const Tuning& a_t, const Tick& a_tick, int a_pickedStep,
             std::span<const WalkNote> a_walk, double a_ms, bool a_explain) {
        Mind& m = MindFor(a_actor);
        Key pick{};   // Idle logs under the default key (the verdict tells it apart from a real attack)
        const bool fired = a_pickedStep >= 0 && static_cast<std::size_t>(a_pickedStep) < a_d.order.size();
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
        if (!ShouldLog(m, pick, a_d.verdict, fired, a_tick.in.now, a_t) && !newWalk) return;
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
        g_readouts[a_actor] = StampedReadout{ std::move(a_r), std::chrono::steady_clock::now() };
    }

    bool ReadReadout(RE::FormID a_actor, Readout& a_out, double* a_ageSec) {
        std::scoped_lock lk(g_readoutMx);
        const auto it = g_readouts.find(a_actor);
        if (it == g_readouts.end()) return false;
        a_out = it->second.r;
        if (a_ageSec)
            *a_ageSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - it->second.at).count();
        return true;
    }

    void EndDecision(RE::FormID a_actor) {
        if (const auto it = g_minds.find(a_actor); it != g_minds.end()) Iaus::EndDecision(it->second);
    }

    void Forget(RE::FormID a_actor) {
        if (const auto it = g_minds.find(a_actor); it != g_minds.end()) {
            Retire(it->second);
            g_minds.erase(it);
        }
        g_budgetWarnedAt.erase(a_actor);
        g_lastWalk.erase(a_actor);
        std::scoped_lock lk(g_readoutMx);
        g_readouts.erase(a_actor);
    }

    bool NoteSource(RE::FormID a_actor, bool a_iaus) {
        auto [it, fresh] = g_source.try_emplace(a_actor, a_iaus);
        if (fresh || it->second == a_iaus) return false;
        it->second = a_iaus;
        return true;
    }

    std::vector<RE::FormID> Noted() {
        std::vector<RE::FormID> out;
        out.reserve(g_source.size());
        for (const auto& kv : g_source) out.push_back(kv.first);
        return out;
    }

    void Dismiss(RE::FormID a_actor) {
        Forget(a_actor);
        g_source.erase(a_actor);
        for (std::uint64_t ch = 0; ch < 8; ++ch) g_paced.erase((static_cast<std::uint64_t>(a_actor) << 8) | ch);
    }

    bool Paced(RE::FormID a_actor, std::uint8_t a_channel, double a_sec, const Tick& a_tick) {
        const std::uint64_t key = (static_cast<std::uint64_t>(a_actor) << 8) | (a_channel & 7u);
        auto [it, fresh] = g_paced.try_emplace(key, a_tick.in.now);
        if (!fresh && a_tick.in.now - it->second < a_sec) return false;
        it->second = a_tick.in.now;
        return true;
    }

    void Clear() {
        for (const auto& kv : g_minds) Retire(kv.second);   // g_epochFloor itself is never reset
        g_minds.clear();
        g_source.clear();
        g_paced.clear();
        g_overCapWarned.clear();
        g_badFactWarned.clear();
        g_budgetWarnedAt.clear();
        g_lastWalk.clear();
        std::scoped_lock lk(g_readoutMx);
        g_readouts.clear();
    }

}
