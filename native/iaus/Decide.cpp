// iaus/Decide.cpp -- one tick's decision (design 4.4-4.8 and 5.1): ranking with ties, momentum, the commitment
// window, the act floor, the rapport-sharpness pick, the walk order, and the Mind bookkeeping. PURE (iaus/Core.h).
#include "iaus/Iaus_internal.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace MFO::Iaus {

    namespace {
        // A small descending list of the best scores seen so far (the P5 cutoff reads its last entry).
        struct TopK {
            std::array<float, 16> v{};
            std::size_t cap = 1, n = 0;
            void Add(float s) {
                std::size_t i = n < cap ? n++ : cap;
                if (i == cap) { if (s <= v[cap - 1]) return; i = cap - 1; }
                while (i > 0 && v[i - 1] < s) { v[i] = v[i - 1]; --i; }
                v[i] = s;
            }
            float Kth() const { return n == cap ? v[cap - 1] : 0.0f; }
        };

        auto TieKey(const Candidate& c) {
            return std::make_tuple(static_cast<int>(c.family), static_cast<int>(c.f.posture), c.form, c.target);
        }
    }

    int RankOf(std::uint32_t e) {
        constexpr std::uint32_t kMaxEpoch = static_cast<std::uint32_t>(kRankBase) - 1000u;   // stays far above 64 rows
        return kRankBase - static_cast<int>((std::min)(e, kMaxEpoch));
    }

    float Unit(std::uint64_t seed, std::uint32_t epoch) {
        // splitmix64 of (seed, epoch): a pure function, so a decision re-rolled on the next tick (the pick was
        // transparent, nothing fired, the epoch did not advance) lands on the SAME pick -- no dither.
        std::uint64_t z = seed ^ (static_cast<std::uint64_t>(epoch) * 0x9E3779B97F4A7C15ull) ^ 0xD1B54A32D192ED03ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<float>(z >> 40) / 16777216.0f;   // 24 bits -> [0,1)
    }

    std::size_t SharpPick(std::span<const float> s, const Sharpness& sh, float u) {
        if (s.empty()) return 0;
        const float best = s[0];
        const std::size_t k = (std::max)(1, sh.k);
        std::size_t pool = 0;
        for (std::size_t i = 0; i < s.size() && pool < k; ++i) {
            if (s[i] >= sh.floor * best - 1e-6f) pool = i + 1;
            else break;
        }
        if (pool <= 1 || !(sh.tau > 0.0f)) return 0;
        std::array<float, 16> w{};
        pool = (std::min)(pool, w.size());
        float sum = 0.0f;
        for (std::size_t i = 0; i < pool; ++i) { w[i] = std::exp((s[i] - best) / sh.tau); sum += w[i]; }
        float acc = 0.0f;
        const float target = u * sum;
        for (std::size_t i = 0; i < pool; ++i) {
            acc += w[i];
            if (target < acc) return i;
        }
        return pool - 1;
    }

    Decision Decide(const Mind& m, std::span<const Candidate> cands, const Profile& p, const Tuning& t,
                    const TickIn& in) {
        Decision d;
        d.generated = cands.size();
        d.rapportRank = in.rapportRank;
        const std::size_t lim = (std::min)(cands.size(), static_cast<std::size_t>((std::max)(0, t.maxCandidates)));
        d.overCap = cands.size() > lim;
        const Sharpness& sh = t.sharp[detail::RankRow(in.rapportRank)];

        TopK top;
        top.cap = (std::min)(std::size_t{ 16 }, static_cast<std::size_t>((std::max)({ 1, sh.k, t.maxTries })));
        d.ranked.reserve(32);

        auto scoreOne = [&](std::size_t i, float bestHeal, bool allowPrune) {
            const Candidate& c = cands[i];
            const bool running = m.hasRunning && KeyOf(c) == m.running;
            const float mom = running ? t.momentum : 1.0f;
            // P5 cutoff: the act floor, or the keep-th best so far (keep = max(k, maxTries): the walk needs the
            // top maxTries for its transparent fall-backs, not just the rapport pool).
            const float cutoff = allowPrune ? (std::max)(t.actFloor, top.Kth()) : 0.0f;
            const auto o = detail::Score(c, m, p, t, in.rapportRank, bestHeal, mom, cutoff, nullptr);
            if (o.vetoed) { ++d.vetoed; return; }
            if (o.pruned) { ++d.pruned; return; }
            ++d.evaluated;
            if (c.family == Family::CastHeal) d.bestHeal = (std::max)(d.bestHeal, o.score / mom);
            if (o.score < t.actFloor) return;
            top.Add(o.score);
            d.ranked.push_back(Scored{ static_cast<std::uint16_t>(i), o.score, o.score / mom, o.focus, running });
        };

        // Pass 1: everything but the heal-dependent families. Heals are never pruned: Retreat and the health
        // drink read the best heal score, so it must be exact.
        for (std::size_t i = 0; i < lim; ++i) {
            const Family f = cands[i].family;
            if (f == Family::Posture) { ++d.vetoed; continue; }   // posture is DecidePosture's layer
            if (detail::DependsOnHeal(f)) continue;
            scoreOne(i, 0.0f, f != Family::CastHeal);
        }
        // Pass 2: Retreat, DrinkHealth.
        for (std::size_t i = 0; i < lim; ++i)
            if (detail::DependsOnHeal(cands[i].family)) scoreOne(i, d.bestHeal, true);

        // Rank, then break ties (4.8): within |dS| < tieEps of a cluster head, the running decision, then the
        // current target, then the lower magicka cost, then the lower (family, posture, form, target) key.
        auto& r = d.ranked;
        std::sort(r.begin(), r.end(), [&](const Scored& a, const Scored& b) {
            if (a.score != b.score) return a.score > b.score;
            return TieKey(cands[a.idx]) < TieKey(cands[b.idx]);
        });
        for (std::size_t i = 0; i < r.size();) {
            std::size_t j = i + 1;
            while (j < r.size() && r[i].score - r[j].score < t.tieEps) ++j;
            if (j - i > 1) {
                std::sort(r.begin() + static_cast<std::ptrdiff_t>(i), r.begin() + static_cast<std::ptrdiff_t>(j),
                          [&](const Scored& a, const Scored& b) {
                              const Candidate& ca = cands[a.idx];
                              const Candidate& cb = cands[b.idx];
                              if (a.running != b.running) return a.running;
                              if (ca.f.currentTarget != cb.f.currentTarget) return ca.f.currentTarget;
                              if (ca.f.cost != cb.f.cost) return ca.f.cost < cb.f.cost;
                              return TieKey(ca) < TieKey(cb);
                          });
            }
            i = j;
        }

        if (r.empty()) { d.verdict = Verdict::Idle; return d; }   // (4.6) nothing above the floor

        d.proposedEpoch = m.epoch + 1u;
        int runPos = -1;
        for (std::size_t i = 0; i < r.size(); ++i)
            if (r[i].running) { runPos = static_cast<int>(i); break; }

        const float best = r[0].score;
        std::size_t pool = 0;
        for (std::size_t i = 0; i < r.size() && pool < static_cast<std::size_t>((std::max)(1, sh.k)); ++i) {
            if (r[i].score >= sh.floor * best - 1e-6f) pool = i + 1;
            else break;
        }

        auto stepFor = [&](std::size_t pos) {
            const bool running = r[pos].running;
            const std::uint32_t e = running ? m.epoch : d.proposedEpoch;
            return Step{ r[pos].idx, RankOf(e), e, running };
        };

        std::size_t pick = 0;
        if (runPos >= 0 && static_cast<std::size_t>(runPos) < pool) {
            // The running decision is still in the rapport pool: keep it, no new roll (5.1 "rolled once per NEW
            // decision", so the follower does not dither).
            pick = static_cast<std::size_t>(runPos);
        } else {
            // A new decision. Inside the commitment window (4.5) only a candidate beating the running one by
            // the preempt margin may take over; with none, the scan holds ("suppressed").
            const bool inWindow = runPos >= 0 && (in.now - m.firedAt) < in.commitWindowSec;
            std::vector<std::size_t> eligible;
            std::vector<float> scores;
            const float bar = inWindow ? r[static_cast<std::size_t>(runPos)].score * (1.0f + t.preemptMargin) : -1.0f;
            for (std::size_t i = 0; i < pool; ++i) {
                if (inWindow && !(r[i].score > bar)) continue;
                eligible.push_back(i);
                scores.push_back(r[i].score);
            }
            if (eligible.empty()) {
                d.verdict = Verdict::Hold;
                d.order.push_back(stepFor(static_cast<std::size_t>(runPos)));
                return d;
            }
            d.rolled = scores.size() > 1 && sh.tau > 0.0f;
            d.roll = Unit(in.seed, d.proposedEpoch);
            pick = eligible[SharpPick(scores, sh, d.roll)];
            if (!d.rolled) d.roll = -1.0f;
            d.newDecision = true;
        }

        d.verdict = Verdict::Act;
        d.order.push_back(stepFor(pick));
        for (std::size_t i = 0; i < r.size() && d.order.size() < static_cast<std::size_t>((std::max)(1, t.maxTries)); ++i)
            if (i != pick) d.order.push_back(stepFor(i));
        return d;
    }

    int DecidePosture(const Mind& m, std::span<const Candidate> cands, const Profile& p, const Tuning& t,
                      std::uint8_t rank, float* outScore) {
        int bestIdx = -1;
        float best = 0.0f;
        bool bestRunning = false;
        for (std::size_t i = 0; i < cands.size(); ++i) {
            const Candidate& c = cands[i];
            if (c.family != Family::Posture) continue;
            // The drawn category ("keep") carries the swap momentum; so does the category posture last fired.
            const bool running = c.f.postureCurrent || (m.hasPosture && KeyOf(c) == m.posture);
            const float mom = running ? t.postureMomentum : 1.0f;
            const auto o = detail::Score(c, m, p, t, rank, 0.0f, mom, 0.0f, nullptr);
            if (o.vetoed) continue;
            // Best wins; within tieEps the running category wins; otherwise the earlier candidate stays.
            const bool better = bestIdx < 0 || o.score > best + t.tieEps ||
                                (std::abs(o.score - best) < t.tieEps && running && !bestRunning);
            if (better) { best = o.score; bestIdx = static_cast<int>(i); bestRunning = running; }
        }
        if (bestIdx >= 0 && best < t.actFloor) bestIdx = -1;
        if (outScore) *outScore = bestIdx >= 0 ? best : 0.0f;
        return bestIdx;
    }

    void OnFired(Mind& m, const Candidate& c, const Step& s, double now) {
        const Key k = KeyOf(c);
        if (!(m.hasRunning && m.running == k)) {
            m.hasRunning = true;
            m.running = k;
            m.decidedAt = now;
            m.epoch = s.epoch;
        }
        m.firedAt = now;   // every Fired re-anchors the window, as today's #26 window does (Scheduler recent.until)
        if (c.form != 0) {
            m.recent[m.recentHead] = c.form;
            m.recentHead = static_cast<std::uint8_t>((m.recentHead + 1u) % m.recent.size());
            if (m.recentN < m.recent.size()) ++m.recentN;
        }
    }

    void OnPostureFired(Mind& m, const Candidate& c) {
        m.hasPosture = true;
        m.posture = KeyOf(c);
    }

    void EndDecision(Mind& m) {
        m.hasRunning = false;
        m.firedAt = -1.0e300;
    }

}
