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
            // P5 cutoff: the act floor, or the keep-th best so far minus tieEps (keep = max(k, maxTries): the walk
            // needs the top maxTries for its transparent fall-backs, not just the rapport pool; the tieEps slack
            // because a tie cluster can promote a candidate just under the k-th by current target or cost). The
            // RUNNING candidate is never pruned: the commitment window and momentum need its exact score, and a
            // pruned running decision made the verdict depend on candidate order (review round 1, finding 1).
            const bool prune = allowPrune && !running;
            const float cutoff = prune ? (std::max)(t.actFloor, top.Kth() - t.tieEps) : 0.0f;
            const auto o = detail::Score(c, m, p, t, in.rapportRank, bestHeal, mom, cutoff, nullptr);
            if (o.vetoed) {
                ++d.vetoed;
                if (o.badFact) { ++d.badFacts; if (d.firstBadFact < 0) d.firstBadFact = static_cast<int>(i); }
                return;
            }
            if (o.pruned) { ++d.pruned; return; }
            ++d.evaluated;
            if (detail::IsHeal(c)) d.bestHeal = (std::max)(d.bestHeal, o.score / mom);
            if (o.score < t.actFloor) return;
            top.Add(o.score);
            d.ranked.push_back(Scored{ static_cast<std::uint16_t>(i), o.score, o.score / mom, o.focus, running });
        };

        // Pass 1: everything but the heal-dependent families. Heals (spells and heal scrolls) are never pruned:
        // Retreat and the health drink read the best heal score, so it must be exact.
        for (std::size_t i = 0; i < lim; ++i) {
            const Family f = cands[i].family;
            if (f == Family::Posture) { ++d.vetoed; continue; }   // posture is DecidePosture's layer
            if (detail::DependsOnHeal(f)) continue;
            scoreOne(i, 0.0f, !detail::IsHeal(cands[i]));
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

        // Stickiness (design 4.4/4.5, review round 1 finding 3): momentum ALWAYS (it is inside S), plus the
        // preempt margin INSIDE the commitment window only. Outside the window every tick is a normal 5.1 rapport
        // pick over the pool; the roll is a pure function of (seed, proposed epoch), so it repeats until a new
        // decision fires (no dither), and landing on the running candidate simply continues it.
        const bool inWindow = runPos >= 0 && (in.now - m.firedAt) < in.commitWindowSec;
        std::vector<std::size_t> eligible;
        std::vector<float> scores;
        float bar = -1.0f;
        if (inWindow) bar = r[static_cast<std::size_t>(runPos)].score * (1.0f + t.preemptMargin);
        for (std::size_t i = 0; i < pool; ++i) {
            if (inWindow && (static_cast<int>(i) == runPos || !(r[i].score > bar))) continue;
            eligible.push_back(i);
            scores.push_back(r[i].score);
        }
        std::size_t pick = 0;
        if (eligible.empty()) {
            // Only reachable inside the window: nothing clears the bar. The running decision continues if it is
            // still the best; otherwise the scan holds ("suppressed", the #26 analogue).
            if (runPos == 0) {
                pick = 0;
            } else {
                d.verdict = Verdict::Hold;
                d.order.push_back(stepFor(static_cast<std::size_t>(runPos)));
                return d;
            }
        } else {
            d.rolled = scores.size() > 1 && sh.tau > 0.0f;
            d.roll = Unit(in.seed, d.proposedEpoch);
            pick = eligible[SharpPick(scores, sh, d.roll)];
            if (!d.rolled) d.roll = -1.0f;
            d.newDecision = !r[pick].running;
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
            // THE WINDOW IS ANCHORED ONCE, WHEN A NEW DECISION FIRES (marth Q67, closes MFO-B651). A repeat Fired of
            // the SAME decision does not re-anchor it: a decision that keeps firing (an attack re-latched, a
            // stream re-driven) used to keep the window open for good, so a waiting heal had to beat it by the
            // preempt margin on every tick (sim: 5-44% of hurt-with-a-heal ticks were Hold). After the window a
            // running decision is held by momentum alone. No emergency margin skip (same ruling).
            m.firedAt = now;
        }
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
