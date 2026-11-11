#pragma once
// iaus/Generate.h -- the PURE candidate generator (card I4, design sections 1.6 and 2). LandingCrew's idea.
//
// Generate(snapshot, classGate, tuning) -> the candidate sets iaus/Core.h scores. Standard headers only, no RE::
// type, no engine call, no global, no clock: the harness and the planned combat simulator call exactly this.
// The engine side (iaus/Candidates.cpp) only fills the Snapshot.
//
// FAMILIES GENERATED: attack, power attack, taunt, retreat, the three drinks, posture (its own list, for
// DecidePosture), cast offense / control / heal / buff, summon, ward.
// NOT generated here: sneak (G1 has not landed), scroll use (card S3: scrolls are IN the book, with their count
// and nature, but no candidate is made), exploit (I3, a gambit row), bash (no action exists).
//
// NEVER GENERATED (design section 2): a spell only the unanimated road can cast, runes and every other target
// location spell, constant effects, a spell whose nature the record does not state, a non-hostile actor as a foe
// (the gather's P3 gates), the player or a party member as the target of a hostile spell (foes only ever come
// from Snapshot::foes, recipients only from self + allies).
//
// PRUNING: P1 static (SpellEntry::drop, class-allowed, a two-hander while a hand is held), P2 resource (cost over
// the pool drops every pair of the spell; no stock drops a drink), P3 the top 5 foes by Prune::FoeRelevance.
// P4 / P5 are the scorer's.
//
// FACTS: only the facts a family's considerations read are written; every other field keeps its neutral default.
// Every ratio goes through Prune::SafeRatio and every copied float through Prune::Finite, so a candidate never
// carries a NaN / inf fact (FactsFinite would veto it). An actor whose maximum reads 0 is skipped, never divided.
//
// LIFETIME: Candidate::formName / targetName are string_views INTO THE SNAPSHOT. Keep the snapshot alive, and do
// not modify it, for as long as the candidates are used.
#include "iaus/Core.h"
#include "iaus/Snapshot.h"

#include <array>
#include <span>
#include <vector>

namespace MFO::Iaus {

    struct Generated {
        std::vector<Candidate> actions;    // for Decide (every family except Posture)
        std::vector<Candidate> postures;   // for DecidePosture
        float worstPartyHp = 1.0f;         // the lowest readable HP pct of self, the player and the allies
        float emergency = 0.0f;            // EmergencyLevel(worstPartyHp)
        bool  selfUnreadable = false;      // the follower's own maxima read 0: nothing was generated
        std::size_t foesSeen = 0;          // readable foes before the top-5 cut
        std::size_t foesKept = 0;
        std::array<std::uint16_t, kFamilies> perFamily{};   // candidates per family (the over-cap WARN, tests)
        // P1 / P2 drops, for the generation log line: spells dropped as unaffordable, off-class, two-handed with a
        // hand held; pairs dropped by a target rule (undead only, already active, live summon).
        std::uint16_t droppedCost = 0, droppedClass = 0, droppedHand = 0, droppedTarget = 0;
    };

    [[nodiscard]] Generated Generate(const Snapshot& a_s, const ClassGate& a_gate,
                                     const Tuning& a_t = DefaultTuning());

    // G10 (design 1.5): the (spell, target) pairs of the first a_n ranked cast-offense / cast-control candidates of
    // a decision. The engine side asks cast/WillWork for these only, so the next lap has their answers.
    struct SpellTarget {
        std::uint32_t spell = 0;
        std::uint32_t target = 0;
        bool operator==(const SpellTarget&) const = default;
    };
    [[nodiscard]] std::vector<SpellTarget> TopSpellPairs(const Decision& a_d, std::span<const Candidate> a_cands,
                                                          std::size_t a_n = 6);

    // G10's ASK LIST, with a memory for refusals (sim finding D1). The top pairs alone forget a refusal every
    // second lap: a pair answered No is vetoed by the WillWork gate, so the decision does not rank it, so it is
    // not asked on the next lap, so it has no answer, and an unanswered pair is Unknown, which passes. A refused
    // pair therefore stays ON the list, and keeps being asked, for as long as its last No is younger than
    // a_keepNoSec. The engine side stamps lastNo each lap the answer is still No, so a standing refusal never
    // lapses; a Yes clears it. PURE (the harness checks it).
    struct AskedPair {
        SpellTarget pair;
        bool        refused = false;   // its last answer was No
        double      lastNo = 0.0;      // when (seconds, the caller's monotonic clock)
        bool operator==(const AskedPair&) const = default;
    };
    // a_prev: the list that was asked this lap, with the answers written back. a_top: this decision's top pairs.
    // Result: a_top first (a pair that is also a live refusal keeps its stamp), then the other live refusals,
    // newest No first, a_cap entries at most (the oldest refusals are dropped first, never a top pair).
    [[nodiscard]] std::vector<AskedPair> NextAskList(std::span<const AskedPair> a_prev,
                                                     std::span<const SpellTarget> a_top, double a_now,
                                                     double a_keepNoSec, std::size_t a_cap = 24);

}
