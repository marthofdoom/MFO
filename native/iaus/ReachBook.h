#pragma once
// iaus/ReachBook.h -- THE ONE READER OF MELEE REACH (marth 2026-11-17: "Yes on, it will need to read each animation too.
// since some have different reach", "some lists (both our testing ones) have combos on light attacks, and maybe on heavy,
// and dont allow adjustment during. so it will be the sequence thats tracked", and the ruling "Edge reach + split uses").
//
// WHAT IS LEARNED. iaus/Reach.h is the estimator (one number from hits and misses). This file is WHAT IS KEYED and WHO READS IT:
//   * EDGE REACH. Every distance the learners see is to the foe's EDGE (centre distance minus the foe's body radius), so a
//     giant's long centre distance does not inflate the reach against a human. Readers add the foe's radius back.
//   * a follower's WEAPON has one learner (the plain per-weapon value: NO lunge). It takes the outcomes of single swings
//     (a chain of one) and of chains whose swings could not be counted, the closest observable thing to "one blow".
//   * a SEQUENCE of that weapon has its own learner: the chain of swings from the first blow until the chain ends (a combo
//     the list does not let him steer once started), keyed by the NUMBER OF SWINGS in the chain. It takes every chain of that
//     length and is SEEDED from the weapon's value at its first outcome. Its value is how far a chain STARTED from there
//     reaches, the lunge of its steps included. (Light / power are NOT keyed apart: no graph tag has been proven to tell a
//     power chain from a light one, and the hit flag is asymmetric, so a power key would only ever see hits. SwingSeq logs the
//     tags seen per chain so a field run can prove one; then the key gets a kind back.)
//   FALLBACK CHAIN, for every read: sequence -> weapon -> fMeleeReach.
//
// THE TWO USES (principle 12: one shared reader, a new reach source is a table row):
//   Use::Blow   "can ONE blow land right now" and the HEIGHT line (StrikeReady, MeleeUnreachableNow, OutOfMeleeReach): the
//               plain per-weapon edge value, never the max over chains (a lunge does not make a foe 450 units up reachable).
//   Use::Combo  "should I start a combo / swing here" and the approach (the attack's range test, the swing gate of the power
//               attack and the bash, the executors' close-or-swing test): the max over the sequences and the weapon.
//   CentreReach(follower, use, foeRadius, flat) adds the foe's radius back and answers the flat fMeleeReach while nothing is learned.
//   Published(follower)   the two edge values the worker last published for him, readable from ANY thread (a leaf mutex; it is
//                         taken inside the combat group's read lock by CombatSense::OutOfMeleeReach and nothing is taken under it).
// To add a reach source: add a key to Book::Observe and a term to the use it belongs to; nothing else reads the learners.
//
// PURE (standard headers only), so the harness runs the very code the game runs. The book is the WORKER domain (#4); only
// the published map is shared.
#include "iaus/Reach.h"
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace MFO::Iaus::ReachBook {

    inline constexpr std::uint8_t kMaxSteps = 15;   // a chain longer than this counts as this many
    inline constexpr float kRefRadius = 22.0f;      // a humanoid body's radius: the seed's conversion from fMeleeReach (centre to centre) to an edge, and the radius of a foe whose own is unread

    [[nodiscard]] inline std::uint8_t SeqKey(unsigned a_steps) { return static_cast<std::uint8_t>((std::min)(a_steps, static_cast<unsigned>(kMaxSteps))); }

    // A chain SPLIT at an instant (review N1): the number of the swings counted so far (a_swings, the sink's monotonic counter) that
    // came AFTER a_splitMs, from the ring of the last 8 swing times (swing n is a_ts[(n - 1) % 8]). Capped at 8; the swings of the
    // old chain are the total minus this. A swing whose slot was already overwritten counts as before the split.
    [[nodiscard]] inline unsigned SwingsAfter(const std::int64_t* a_ts, std::uint32_t a_swings, std::int64_t a_splitMs) {
        unsigned n = 0;
        for (std::uint32_t k = 0; k < (std::min)(a_swings, 8u); ++k)
            if (a_ts[(a_swings - 1u - k) % 8u] > a_splitMs) ++n;
        return n;
    }
    // A hit belongs to the chain whose window it falls in: since a_windowStart, and (a split chain) up to a_endAt (0 = no end).
    [[nodiscard]] inline bool InChainWindow(double a_t, double a_windowStart, double a_endAt) {
        return a_t != 0.0 && a_t >= a_windowStart && (a_endAt == 0.0 || a_t <= a_endAt);
    }

    struct WeaponReach {
        Reach::Learner weapon;                                  // the per-weapon edge value (no lunge)
        std::unordered_map<std::uint8_t, Reach::Learner> seq;   // by SeqKey (swing count), edge values
    };

    class Book {
    public:
        // One outcome: a chain of a_steps swings (0 = not countable) that began a_edgeDist from the foe's edge, and whether a blow
        // of it hit. a_seedEdge = fMeleeReach - kRefRadius, the weapon learner's start.
        void Observe(std::uint32_t a_follower, std::uint32_t a_weapon, float a_seedEdge, unsigned a_steps, float a_edgeDist, bool a_hit) {
            if (m_map.size() > kBound) m_map.clear();   // a bound, never reached by a real party
            WeaponReach& w = Entry(a_follower, a_weapon, a_seedEdge);
            if (a_steps > 0) {
                const std::uint8_t k = SeqKey(a_steps);
                auto it = w.seq.find(k);
                if (it == w.seq.end()) {   // the per-weapon value is the seed
                    Reach::Learner s; s.Reset(w.weapon.Known() ? w.weapon.Value() : a_seedEdge);
                    it = w.seq.emplace(k, s).first;
                }
                it->second.Observe(a_edgeDist, a_hit);
            }
            if (a_steps <= 1) w.weapon.Observe(a_edgeDist, a_hit);   // a single swing: the closest thing to "one blow, no lunge"
        }
        // Make the weapon's entry exist (so the readers have a value before his first chain ends).
        void Ensure(std::uint32_t a_follower, std::uint32_t a_weapon, float a_seedEdge) { (void)Entry(a_follower, a_weapon, a_seedEdge); }

        [[nodiscard]] const WeaponReach* Find(std::uint32_t a_follower, std::uint32_t a_weapon) const {
            const auto it = m_map.find(Key(a_follower, a_weapon));
            return it == m_map.end() ? nullptr : &it->second;
        }
        // Use::Blow: the plain per-weapon edge value. 0 = nothing learned.
        [[nodiscard]] float BlowEdge(std::uint32_t a_follower, std::uint32_t a_weapon) const {
            const WeaponReach* w = Find(a_follower, a_weapon);
            return (w && w->weapon.Known()) ? w->weapon.Value() : 0.0f;
        }
        // Use::Combo: the max over the weapon's sequences and the weapon itself. 0 = nothing learned.
        [[nodiscard]] float ComboEdge(std::uint32_t a_follower, std::uint32_t a_weapon) const {
            const WeaponReach* w = Find(a_follower, a_weapon);
            if (!w || !w->weapon.Known()) return 0.0f;
            float r = w->weapon.Value();
            for (const auto& [k, l] : w->seq) if (l.Known()) r = (std::max)(r, l.Value());
            return r;
        }
        void Clear() { m_map.clear(); }
        [[nodiscard]] std::size_t Size() const { return m_map.size(); }

    private:
        static constexpr std::size_t kBound = 512;
        [[nodiscard]] static std::uint64_t Key(std::uint32_t a_follower, std::uint32_t a_weapon) { return (static_cast<std::uint64_t>(a_follower) << 32) | a_weapon; }
        WeaponReach& Entry(std::uint32_t a_follower, std::uint32_t a_weapon, float a_seedEdge) {
            WeaponReach& w = m_map[Key(a_follower, a_weapon)];
            if (!w.weapon.Known()) w.weapon.Reset(a_seedEdge);
            return w;
        }
        std::unordered_map<std::uint64_t, WeaponReach> m_map;
    };

    // ── the published edge values: ANY thread may read them ────────────────────────────────────────────────────────────
    enum class Use : std::uint8_t { Blow = 0, Combo = 1 };
    struct Pub { float blow = 0.0f, combo = 0.0f; };
    struct Published_ {
        std::mutex mx;
        std::unordered_map<std::uint32_t, Pub> by;
    };
    [[nodiscard]] inline Published_& Shared() { static Published_ s; return s; }
    // Both 0 erases (a bow in hand, the fight ended): every reader then answers the flat fMeleeReach.
    inline void Publish(std::uint32_t a_follower, float a_blowEdge, float a_comboEdge) {
        std::scoped_lock lk(Shared().mx);
        if (a_blowEdge > 0.0f || a_comboEdge > 0.0f) Shared().by[a_follower] = Pub{ a_blowEdge, a_comboEdge }; else Shared().by.erase(a_follower);
    }
    [[nodiscard]] inline Pub Published(std::uint32_t a_follower) {
        std::scoped_lock lk(Shared().mx);
        const auto it = Shared().by.find(a_follower);
        return it == Shared().by.end() ? Pub{} : it->second;
    }
    inline void ClearPublished() { std::scoped_lock lk(Shared().mx); Shared().by.clear(); }

    // THE reader behind every centre-to-centre reach question about a_foe (a_foeRadius <= 0 = unread: the humanoid kRefRadius).
    // a_flat = fMeleeReach, answered while nothing is learned for him.
    [[nodiscard]] inline float CentreReach(float a_blowEdge, float a_comboEdge, Use a_use, float a_foeRadius, float a_flat) {
        const float edge = a_use == Use::Blow ? a_blowEdge : a_comboEdge;
        return edge > 0.0f ? edge + (a_foeRadius > 0.0f ? a_foeRadius : kRefRadius) : a_flat;
    }
    [[nodiscard]] inline float CentreReach(std::uint32_t a_follower, Use a_use, float a_foeRadius, float a_flat) {
        const Pub p = Published(a_follower);
        return CentreReach(p.blow, p.combo, a_use, a_foeRadius, a_flat);
    }
    // The distance at which a swing he times himself is still worth the swing (the swing gate's veto line: its learned reach x
    // kGate x 1.15), for the executors that close-or-swing (cast/Fire.cpp). The flat fMeleeReach while nothing is learned.
    [[nodiscard]] inline float SwingLimit(std::uint32_t a_follower, float a_foeRadius, float a_flat) {
        const Pub p = Published(a_follower);
        if (!(p.combo > 0.0f)) return a_flat;
        return CentreReach(p.blow, p.combo, Use::Combo, a_foeRadius, a_flat) * Reach::kGate * 1.15f;
    }

}
