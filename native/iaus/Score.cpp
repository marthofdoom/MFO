// iaus/Score.cpp -- the combination (design section 4.1-4.3): per-consideration curve and weight, the compensation
// factor, gates, boosts, class and action weights, player focus, and the P5 early-out. PURE (iaus/Core.h).
#include "iaus/Iaus_internal.h"

#include <algorithm>
#include <cmath>

namespace MFO::Iaus {

    namespace detail {

        float ClassWeight(const Candidate& c, const Profile& p) {
            const auto fi = static_cast<std::size_t>(c.family);
            const auto ri = static_cast<std::size_t>(c.role);
            const float fw = fi < kFamilies ? p.family[fi] : 1.0f;
            const float rw = ri < kRoleTags ? p.role[ri] : 1.0f;
            return fw * rw;
        }

        float WeightFor(Family f, const Use& u, const Profile& p) {
            for (const auto& o : p.weights)
                if (o.family == f && o.cons == u.cons) return o.w;
            return u.w;
        }

        ScoreOut Score(const Candidate& c, const Mind& m, const Profile& p, const Tuning& t, std::uint8_t rank,
                       float bestHeal, float momentum, float cutoff, std::vector<Term>* terms) {
            ScoreOut out;
            const auto set = SetFor(c.family, c.scrollOf);
            if (set.empty()) { out.vetoed = true; return out; }
            // A NaN / inf fact is a reader bug: veto, never drop or pass the term (review round 1, finding 2).
            if (!FactsFinite(c.f)) { out.vetoed = true; out.badFact = true; return out; }

            // Inputs once (every consideration reads cheap, already-read facts). kMaxSet bounds the largest set.
            constexpr std::size_t kMaxSet = 24;
            std::array<In, kMaxSet> xs{};
            const std::size_t ns = (std::min)(set.size(), kMaxSet);
            for (std::size_t i = 0; i < ns; ++i) {
                xs[i] = Input(set[i].cons, c, m, bestHeal, t);
                if (xs[i].on && std::isnan(xs[i].x)) {   // finite facts can still divide into a NaN: same rule
                    out.vetoed = true; out.badFact = true; out.vetoBy = set[i].cons;
                    return out;
                }
            }

            auto curveOf = [&](Cons k) -> const Curve& { return t.curve[static_cast<std::size_t>(k)]; };
            auto veto = [&](Cons k) { out.vetoed = true; out.vetoBy = k; out.score = 0.0f; return out; };

            // (P4) hard gates first, and the boosts: both are one comparison each.
            float boost = 1.0f;
            std::size_t n = 0;
            for (std::size_t i = 0; i < ns; ++i) {
                if (!xs[i].on) continue;
                const Cons k = set[i].cons;
                const float x = xs[i].x;
                switch (RoleOf(k)) {
                case Role::Gate: {
                    const float s = Eval(curveOf(k), x);
                    if (terms) terms->push_back({ k, x, s, 1.0f, s, s });
                    if (!(s > 0.0f)) return veto(k);
                    break;
                }
                case Role::Boost: {
                    const float s = Eval(curveOf(k), x);
                    if (terms && s != 1.0f) terms->push_back({ k, x, s, 1.0f, s, s });
                    boost *= s;
                    break;
                }
                case Role::Shape:
                    ++n;
                    break;
                }
            }

            const auto fi = static_cast<std::size_t>(c.family);
            const float aw = fi < kFamilies ? t.actionWeight[fi] : 0.0f;
            out.focus = PlayerRelevant(c) ? t.focus[RankRow(rank)] : 1.0f;
            const float base = aw * ClassWeight(c, p) * out.focus * boost * momentum;
            if (!(base > 0.0f)) return veto(Cons::kCount);   // a zero weight disables the family

            // (4.2) Dave Mark's compensation: mod = 1 - 1/n, s' = s + (1 - s) * mod * s. Every s' <= 1, so the
            // product only falls as terms multiply in, which is what makes the P5 bound below sound.
            const float mod = n > 0 ? 1.0f - 1.0f / static_cast<float>(n) : 0.0f;
            float prod = 1.0f;
            for (std::size_t i = 0; i < ns; ++i) {
                if (!xs[i].on) continue;
                const Cons k = set[i].cons;
                if (RoleOf(k) != Role::Shape) continue;
                const float x = xs[i].x;
                const float raw = Eval(curveOf(k), x);
                if (!(raw > 0.0f)) {
                    if (terms) terms->push_back({ k, x, raw, WeightFor(c.family, set[i], p), 0.0f, 0.0f });
                    return veto(k);
                }
                const float w = WeightFor(c.family, set[i], p);
                const float s = std::pow((std::min)(raw, 1.0f), w);
                const float comp = s + (1.0f - s) * mod * s;
                prod *= comp;
                if (terms) terms->push_back({ k, x, raw, w, s, comp });
                // (P5) multiplicative early-out against the current k-th best.
                if (!terms && cutoff > 0.0f && base * prod < cutoff) {
                    out.pruned = true;
                    out.score = base * prod;
                    return out;
                }
            }
            out.score = base * prod;
            return out;
        }

    }

    float ScoreOne(const Candidate& c, const Mind& m, const Profile& p, const Tuning& t, std::uint8_t rank,
                   float bestHeal, std::vector<Term>* terms, Cons* vetoBy, float* focus) {
        const auto o = detail::Score(c, m, p, t, rank, bestHeal, 1.0f, 0.0f, terms);
        if (vetoBy) *vetoBy = o.vetoBy;
        if (focus) *focus = o.focus;
        return o.vetoed ? 0.0f : o.score;
    }

}
