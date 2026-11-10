#pragma once
// iaus/Iaus_internal.h -- state shared by the iaus/ core files only (Catalogue, Score, Decide, Explain).
// PURE like Core.h: standard headers only. Nothing outside native/iaus/ (and the harness) includes this.
#include "iaus/Core.h"

#include <cmath>

namespace MFO::Iaus::detail {

    // One consideration in a family's set, with its default weight (the s^w exponent, section 3).
    struct Use { Cons cons; float w; };

    // The consideration set of a candidate's family (Scroll resolves through scrollOf). Catalogue.cpp.
    [[nodiscard]] std::span<const Use> SetFor(Family a_family, Family a_scrollOf);

    // The consideration's input x, read from the candidate's facts (and, for Variety / the heal-dependent
    // vetoes, from the Mind and the tick's best heal score). NaN = NOT APPLICABLE to this candidate: the term is
    // dropped and not counted in n. Catalogue.cpp.
    [[nodiscard]] float Input(Cons a_c, const Candidate& a_cand, const Mind& a_m, float a_bestHeal,
                              const Tuning& a_t);

    // Families whose score depends on the tick's best heal (Retreat, DrinkHealth): scored in a second pass.
    [[nodiscard]] inline bool DependsOnHeal(Family f) {
        return f == Family::Retreat || f == Family::DrinkHealth;
    }

    // Player focus relevance (section 5.2): heals or buffs the player, protects the player, or attacks the
    // player's target. Capped at one application.
    [[nodiscard]] bool PlayerRelevant(const Candidate& a_c);

    [[nodiscard]] inline std::size_t RankRow(std::uint8_t a_rank) {
        return a_rank < 1 ? 0u : (a_rank > 5 ? 4u : static_cast<std::size_t>(a_rank - 1));
    }

    [[nodiscard]] inline bool IsNA(float x) { return std::isnan(x); }

    // classWeight(candidate) = Profile::family[f] x Profile::role[tag] (section 4.9).
    [[nodiscard]] float ClassWeight(const Candidate& a_c, const Profile& a_p);
    // A consideration's weight for this family: the Profile override, else the set's default.
    [[nodiscard]] float WeightFor(Family a_f, const Use& a_u, const Profile& a_p);

    struct ScoreOut {
        float score = 0.0f;              // final (momentum included when a_momentum != 1)
        float focus = 1.0f;
        Cons  vetoBy = Cons::kCount;
        bool  vetoed = false;
        bool  pruned = false;            // P5: stopped once base x partial product fell below a_cutoff
    };
    // The section 4 combination for one candidate. a_cutoff > 0 enables the P5 early-out (never with a_terms).
    [[nodiscard]] ScoreOut Score(const Candidate& a_c, const Mind& a_m, const Profile& a_p, const Tuning& a_t,
                                 std::uint8_t a_rank, float a_bestHeal, float a_momentum, float a_cutoff,
                                 std::vector<Term>* a_terms);

}
