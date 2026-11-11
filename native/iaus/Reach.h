#pragma once
// iaus/Reach.h -- THE SWING REACH, LEARNED FROM WHAT HIT AND WHAT MISSED (ruling Q68, marth 2026-11-11).
// LandingCrew's idea for MFO, as the rest of iaus/.
//
// marth: "precision reach will be different for every single weapon. so find one formula won't work." and "In game I
// would measure by whether it hits or not. A miss that should have hit (based on distance alone) lowers the distance,
// hits distance is also tracked."
//
// So the reach the SWING gate uses (the power attack, later the bash; ruling Q70: never the target choice) is not a
// constant and not a formula. It is one number per (follower, weapon, swing kind) that starts at the best estimate
// there is (Precision's own for that weapon when the bridge answers, else the vanilla fMeleeReach) and then follows
// the outcomes of his committed swings:
//   a HIT at distance d   says the reach is at least d;
//   a MISS at distance d  says the reach is under d, when d was inside the reach he believed in.
//
// THE ESTIMATOR. Keep the last kWindow outcomes and the present value r. After every outcome the new r is the
// distance of the LONGEST HIT THE EVIDENCE SUPPORTS: over every split point, count the misses inside it (weight
// kMissWeight: a whiff wastes the swing, its stamina and its cooldown) and the hits beyond it (weight 1: a swing he
// would not have tried), take the split with the lowest count (on a tie the one nearest the present value), and r is the longest hit under
// that split. The present r itself votes as one hit, which is what makes the value a memory and not only a window: a
// miss that has left the window does not let r jump back up. With no hit under the split r goes just under the
// shortest miss. One outcome moves r by at most kStep of itself, and r never leaves [kFloor, kCap] x the start value.
//   It rises only on a real hit beyond r (the gate lets him try up to 1.15 x r, and the engine's own swings are
//   outcomes too), so it cannot creep. It falls on a miss inside r that no longer hit contradicts. One stray miss
//   (the target stepped aside) is outvoted by the hits recorded beyond it.
//
// NO EXPIRY (principle 9: a floor is safe, an expiry is not). Nothing here reads a clock: the window is counted in
// swings, so a follower who has not swung for a minute knows what he knew.
//
// PURE: standard headers only. The engine side (which events are a committed swing, a hit, a miss "by distance alone")
// is its own card; this file is what the game, the harness and the simulator all run.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace MFO::Iaus::Reach {

    inline constexpr std::size_t kWindow = 12;        // outcomes kept: about 15 s of melee at the measured 1.2 s hit cadence
    inline constexpr float kMissWeight = 1.5f;        // a miss inside the reach against a hit beyond it
    inline constexpr float kFloor = 0.5f;             // of the start value
    inline constexpr float kCap = 2.0f;               // of the start value
    inline constexpr float kGate = 1.05f / 1.15f;     // the gate's reach as a share of the learned value (see Gate())
    inline constexpr float kStep = 0.10f;             // the most one outcome moves the value, as a share of it
    inline constexpr float kUnderMiss = 0.95f;        // with no supporting hit: this far under the shortest miss

    struct Learner {
        float start = 0.0f;                           // the value it began with (0 = never set: Value() answers 0, "unknown")
        float r = 0.0f;                               // the learned reach
        std::array<float, kWindow> dist{};
        std::array<bool, kWindow>  hit{};
        std::uint8_t n = 0, head = 0;
        std::uint32_t hits = 0, misses = 0;           // lifetime counts (log / board)

        void Reset(float a_start) { *this = Learner{}; if (std::isfinite(a_start) && a_start > 0.0f) { start = a_start; r = a_start; } }
        [[nodiscard]] bool  Known() const { return start > 0.0f; }
        [[nodiscard]] float Value() const { return r; }
        // What the swing gate reads as its reach. The range curve (Cons::RangeMelee) is flat to 1.0 x its reach and vetoes
        // above 1.15 x, so the gate's reach is the learned value x kGate: he still tries a little past what he knows
        // (1.15 x kGate = 1.05, which is how the value can grow from his own swings), not 15 percent past it.
        [[nodiscard]] float Gate() const { return r * kGate; }

        // One committed swing: its distance at the swing's start and whether it hit. A non-finite or negative distance
        // is ignored (a reader bug must not move the value).
        void Observe(float a_dist, bool a_hit) {
            if (!Known() || !std::isfinite(a_dist) || a_dist < 0.0f) return;
            dist[head] = a_dist; hit[head] = a_hit; head = static_cast<std::uint8_t>((head + 1) % kWindow); if (n < kWindow) ++n;
            ++(a_hit ? hits : misses);
            r = Solve();
        }

    private:
        // The lowest-cost split over the window plus the present value as one hit. Candidate splits: just above each
        // hit (the reach is that hit), and "under every hit". O(kWindow^2), 12 x 13 comparisons.
        [[nodiscard]] float Solve() const {
            const float lo = start * kFloor, hi = start * kCap;
            auto cost = [&](float a_split) {   // misses at or inside the split, hits beyond it
                float c = r > a_split ? 1.0f : 0.0f;
                for (std::uint8_t i = 0; i < n; ++i) {
                    if (hit[i]) { if (dist[i] > a_split) c += 1.0f; }
                    else if (dist[i] <= a_split) c += kMissWeight;
                }
                return c;
            };
            // Candidates: the present value, every hit in the window, and "under every hit" = just under the shortest miss.
            // A tie goes to the candidate nearest the present value (stability: equal evidence does not move him).
            float best = r, bestCost = cost(r);
            auto consider = [&](float a_split) {
                const float c = cost(a_split);
                if (c < bestCost || (c == bestCost && std::fabs(a_split - r) < std::fabs(best - r))) { best = a_split; bestCost = c; }
            };
            bool  anyMiss = false;
            float shortestMiss = 0.0f;
            for (std::uint8_t i = 0; i < n; ++i) {
                if (hit[i]) consider(dist[i]);
                else { shortestMiss = anyMiss ? (std::min)(shortestMiss, dist[i]) : dist[i]; anyMiss = true; }
            }
            if (anyMiss && shortestMiss * kUnderMiss < r) consider(shortestMiss * kUnderMiss);   // only ever a way DOWN: a miss never raises the value
            // One outcome moves the value by at most kStep of itself: a window that happens to hold three unlucky misses
            // has to keep saying so for several swings before he believes it.
            best = std::clamp(best, r * (1.0f - kStep), r * (1.0f + kStep));
            return std::clamp(best, lo, hi);
        }
    };

}
