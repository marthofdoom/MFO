#pragma once
// iaus/ReachBook.h -- THE ONE READER OF MELEE REACH (marth 2026-11-17: "Yes on, it will need to read each animation too.
// since some have different reach", and "some lists (both our testing ones) have combos on light attacks, and maybe on
// heavy, and dont allow adjustment during. so it will be the sequence thats tracked").
//
// WHAT IS LEARNED. iaus/Reach.h is the estimator (one number from hits and misses). This file is WHAT IS KEYED and WHO READS IT:
//   * a follower's WEAPON has one learner (the per-weapon value, seeded from fMeleeReach);
//   * a SEQUENCE of that weapon has its own learner: the chain of swings from the first blow until the chain ends (a combo
//     the list does not let him steer once started), keyed by (kind: light / power / bash, number of swings in the chain).
//     It is SEEDED from the weapon's value at the moment it is first seen. An outcome is one chain: the distance at the
//     chain's start and whether any blow of it hit. So the learned value is how far a CHAIN started from there reaches,
//     the forward carry of its steps included. The clip of a single step is not observable (the graph raises the same
//     tags for every clip; docs/research/2026-11/anim-reach.md), the chain is the finest identity there is.
//   FALLBACK CHAIN, for every read: sequence -> weapon -> fMeleeReach.
//
// THE READS (principle 12: one shared reader, a new reach source is a table row):
//   Reachability(follower, weapon)   "can he hit it from here": the MAX over his weapon's sequences and the weapon's value
//                                    (the longest thing he is likely to swing). 0 = nothing learned.
//   Gate(follower, weapon, kind)     the swing gate of a swing he times himself (power attack, bash): the max Gate() over
//                                    the sequences of that kind, else the weapon's. 0 = nothing learned.
//   Published(follower)              the Reachability the worker last published for him, readable from ANY thread (a leaf
//                                    mutex; CombatSense::OutOfMeleeReach and the Snapshot's bladeReach read it).
// To add a reach source: add a key to Book::Observe / a term to Reachability, nothing else reads the learners.
//
// PURE (standard headers only), so the harness runs the very code the game runs. The book is the WORKER domain (#4); only
// the published map is shared, and it is a leaf mutex that nothing else is taken under.
#include "iaus/Reach.h"
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace MFO::Iaus::ReachBook {

    enum class Kind : std::uint8_t { Light = 0, Power = 1, Bash = 2 };
    inline constexpr std::uint8_t kMaxSteps = 15;   // a chain longer than this counts as this many

    [[nodiscard]] inline std::uint8_t SeqKey(Kind a_kind, unsigned a_steps) {
        return static_cast<std::uint8_t>((static_cast<unsigned>(a_kind) << 4) | (std::min)(a_steps, static_cast<unsigned>(kMaxSteps)));
    }
    [[nodiscard]] inline Kind KindOf(std::uint8_t a_key) { return static_cast<Kind>(a_key >> 4); }

    struct WeaponReach {
        Reach::Learner weapon;                                  // the per-weapon value
        std::unordered_map<std::uint8_t, Reach::Learner> seq;   // by SeqKey
    };

    class Book {
    public:
        // One outcome: a chain of a_steps swings (0 = the chain was not observable: the weapon learner only) of kind a_kind
        // that began a_dist from the foe, and whether a blow of it hit. a_seed = fMeleeReach, the weapon learner's start.
        void Observe(std::uint32_t a_follower, std::uint32_t a_weapon, float a_seed, Kind a_kind, unsigned a_steps, float a_dist, bool a_hit) {
            if (m_map.size() > kBound) m_map.clear();   // a bound, never reached by a real party
            WeaponReach& w = Entry(a_follower, a_weapon, a_seed);
            if (a_steps > 0) {
                const std::uint8_t k = SeqKey(a_kind, a_steps);
                auto it = w.seq.find(k);
                if (it == w.seq.end()) {   // the per-weapon value is the seed (marth: "starting from the per-weapon value")
                    Reach::Learner s; s.Reset(w.weapon.Known() ? w.weapon.Value() : a_seed);
                    it = w.seq.emplace(k, s).first;
                }
                it->second.Observe(a_dist, a_hit);
            }
            w.weapon.Observe(a_dist, a_hit);
        }
        // Make the weapon's entry exist (so the swing gate and the reader have a value before his first chain ends).
        void Ensure(std::uint32_t a_follower, std::uint32_t a_weapon, float a_seed) { (void)Entry(a_follower, a_weapon, a_seed); }

        [[nodiscard]] const WeaponReach* Find(std::uint32_t a_follower, std::uint32_t a_weapon) const {
            const auto it = m_map.find(Key(a_follower, a_weapon));
            return it == m_map.end() ? nullptr : &it->second;
        }
        [[nodiscard]] float Reachability(std::uint32_t a_follower, std::uint32_t a_weapon) const {
            const WeaponReach* w = Find(a_follower, a_weapon);
            if (!w || !w->weapon.Known()) return 0.0f;
            float r = w->weapon.Value();
            for (const auto& [k, l] : w->seq) if (l.Known()) r = (std::max)(r, l.Value());
            return r;
        }
        [[nodiscard]] float Gate(std::uint32_t a_follower, std::uint32_t a_weapon, Kind a_kind) const {
            const WeaponReach* w = Find(a_follower, a_weapon);
            if (!w || !w->weapon.Known()) return 0.0f;
            float g = 0.0f;
            for (const auto& [k, l] : w->seq) if (l.Known() && KindOf(k) == a_kind) g = (std::max)(g, l.Gate());
            return g > 0.0f ? g : w->weapon.Gate();
        }
        void Clear() { m_map.clear(); }
        [[nodiscard]] std::size_t Size() const { return m_map.size(); }

    private:
        static constexpr std::size_t kBound = 512;
        [[nodiscard]] static std::uint64_t Key(std::uint32_t a_follower, std::uint32_t a_weapon) { return (static_cast<std::uint64_t>(a_follower) << 32) | a_weapon; }
        WeaponReach& Entry(std::uint32_t a_follower, std::uint32_t a_weapon, float a_seed) {
            WeaponReach& w = m_map[Key(a_follower, a_weapon)];
            if (!w.weapon.Known()) w.weapon.Reset(a_seed);
            return w;
        }
        std::unordered_map<std::uint64_t, WeaponReach> m_map;
    };

    // ── the published reach: ANY thread may read it ────────────────────────────────────────────────────────────────────
    struct Published_ {
        std::mutex mx;
        std::unordered_map<std::uint32_t, float> by;
    };
    [[nodiscard]] inline Published_& Shared() { static Published_ s; return s; }
    inline void Publish(std::uint32_t a_follower, float a_reach) {
        std::scoped_lock lk(Shared().mx);
        if (a_reach > 0.0f) Shared().by[a_follower] = a_reach; else Shared().by.erase(a_follower);
    }
    // 0 = nothing learned for him (the caller reads fMeleeReach).
    [[nodiscard]] inline float Published(std::uint32_t a_follower) {
        std::scoped_lock lk(Shared().mx);
        const auto it = Shared().by.find(a_follower);
        return it == Shared().by.end() ? 0.0f : it->second;
    }
    inline void ClearPublished() { std::scoped_lock lk(Shared().mx); Shared().by.clear(); }

    // THE ONE FALLBACK: the reach a reachability question reads for a_follower, a_flat = fMeleeReach.
    [[nodiscard]] inline float ReachOr(std::uint32_t a_follower, float a_flat) {
        const float p = Published(a_follower);
        return p > 0.0f ? p : a_flat;
    }

}
