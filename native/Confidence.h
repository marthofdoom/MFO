#pragma once
#include "PCH.h"
#include "Vocabulary.h"   // HealthPct / StaminaPct / MagickaPct
#include "Config.h"
#include "CombatSense.h"  // FoeLoad -- weighted combat-group foe tally (#23, v2)
#include <array>

// THE CONFIDENCE LEASH -- a CORE TENET (DESIGN.md). marth, 2026-07-29.
//
// A follower's willingness to operate AWAY FROM THE PLAYER is not a fixed
// follow distance -- it is a live readout of how confident that follower is to
// survive on their own right now. Bold in an easy fight (the leash grows, they
// push ahead, engage the enemy nearest THEM, range out to loot); cautious in a
// hard one (the leash shrinks, they fall back and fight at the player's side,
// grabbing only nearby loot). "Nearest enemy" and "loot distance" are still
// measured from the FOLLOWER; the invisible string is how far from the PLAYER
// the follower is willing to be, and it tightens in danger and loosens in
// safety. The player never sees a number -- they just feel it.
//
// ONE PRIMITIVE, TWO CONSUMERS: the loot leash (Logistics -- whether to travel
// to a ref and how far) now, and combat target distance (Targeting/Evaluator --
// how far to chase the nearest foe) next. Neither reinvents "how brave is this
// follower"; both read Of()/LeashRadius().
namespace MFO::Confidence {

    // ── CONFIDENCE v2 (86e3erv94, 2026-09-26) ────────────────────────────────
    // v1 was vitality x a foe-COUNT multiplier, which read foe count, not
    // "losing": five mudcrabs forced a full-health retreat, a dragon weighed the
    // same as a mudcrab, and a losing duel only registered at ~20% HP because
    // nothing watched HP FALL (assess-confidence-leash). v2 is
    //
    //     Of = Vitality x Trend x FoeMultiplier(foe LOAD)        (in combat)
    //     Of = Vitality x Trend                                  (out of combat)
    //
    //  - foe LOAD = the sum of CombatSense::FoeWeight over the combat group's
    //    live foes (0.5 mudcrab .. 1.0 even .. 2.0 dragon); an empty group in
    //    combat still counts one even foe (v1's max(1, foes));
    //  - FoeMultiplier(L) = 0.90 / (1 + 0.25 L), floored at 0.15: an even duel
    //    0.72, two even foes 0.60, five mudcrabs (L 2.5) 0.55, five even foes 0.40,
    //    and it takes ~10 even foes (or 5 dragons) at FULL health to reach the
    //    0.25 retreat floor on the load alone;
    //  - Trend = time-to-death / kTrendTtdSafe, in [kTrendFloor, 1], from the net
    //    HP-loss rate over the last few seconds of this follower's own services
    //    (HpLossRate). A follower losing HP fast reads low BEFORE his HP is low:
    //    a dragon duel lost at 4%/s crosses the retreat floor near 60% HP, at
    //    2%/s near 40%; a follower holding his own reads Trend 1.
    // Target behaviours + the offline before/after table: MAP.md (Confidence).

    // The vitality term of Of() (no fight multiplier, no trend), unclamped.
    inline float Vitality(RE::Actor* a_follower) {
        const float hp = Vocab::HealthPct(a_follower);
        const float st = Vocab::StaminaPct(a_follower);
        const float mg = Vocab::MagickaPct(a_follower);
        return 0.6f * hp + 0.2f * st + 0.2f * mg;
    }

    // The fight multiplier for a WEIGHTED foe load (one even foe = 1.0). ONE
    // definition, shared by Of() and OfFacing() so the estimate cannot drift
    // from the real read.
    inline constexpr float kFightMultMax  = 0.90f;
    inline constexpr float kFightMultMin  = 0.15f;
    inline constexpr float kFightLoadStep = 0.25f;
    inline float FoeMultiplier(float a_load) {
        return std::clamp(kFightMultMax / (1.0f + kFightLoadStep * std::max(0.0f, a_load)),
                          kFightMultMin, kFightMultMax);
    }

    // ── HP TREND (principle 9: sized from the real service cadence) ──────────
    // One small ring of (service-clock seconds, HealthPct) per follower, pushed
    // by NoteHealth once per OWN service (Scheduler::Tick, worker, both tables,
    // after the pause gate), so its cadence is his service period N x 133 ms
    // (N = active roster). Pushes closer than kTrendMinStep are dropped, so the
    // kTrendRing entries always span >= kTrendRing x kTrendMinStep = 8 s -- past
    // the kTrendWindow read at any roster size. The read window is
    // max(kTrendWindow, 2.5 x the newest gap): at N = 20 (2.7 s period) a flat
    // 5 s window would hold one sample and read no trend at all, so it widens
    // with the observed cadence instead of expiring live data. A gap longer than
    // kTrendMaxGap (unpaused service time: dismissal, a long absence) or a clock
    // that ran backwards (revert reset g_serviceClock) restarts the ring.
    // Rate = the NET loss from the oldest in-window sample to the newest over
    // their span (a heal offsets the damage it undoes; rising HP reads 0), only
    // once the span is >= kTrendMinSpan so a single fresh hit cannot spike it.
    // THREADING: pushed on the worker; read wherever Of() is read. A leaf mutex
    // guards the map (nothing is called under it, and it is never taken with the
    // combat-group lock held), so an off-worker Of() reader stays sound.
    inline constexpr std::size_t kTrendRing     = 32;
    inline constexpr double      kTrendMinStep  = 0.25;   // s between kept samples
    inline constexpr double      kTrendWindow   = 5.0;    // s, the base read window
    inline constexpr double      kTrendMinSpan  = 1.0;    // s, shortest span that yields a rate
    inline constexpr double      kTrendMaxGap   = 10.0;   // s, a gap past this restarts the ring
    inline constexpr float       kTrendTtdSafe  = 20.0f;  // s to death at which Trend reaches 1
    inline constexpr float       kTrendFloor    = 0.30f;  // Trend never drops below this
    inline constexpr float       kTrendRateEps  = 0.002f; // /s (0.2%/s): below this = not losing

    namespace detail {
        struct HpSample { double t = 0.0; float hp = 1.0f; };
        struct HpRing {
            std::array<HpSample, kTrendRing> s{};
            std::size_t head  = 0;   // index of the NEWEST sample
            std::size_t count = 0;
        };
        inline std::mutex                             g_trendLock;
        inline std::unordered_map<RE::FormID, HpRing> g_trend;
    }

    // Worker, once per own service: record his health on the unpaused service clock.
    inline void NoteHealth(RE::FormID a_id, float a_hpPct, double a_clock) {
        std::lock_guard lk(detail::g_trendLock);
        auto& r = detail::g_trend[a_id];
        if (r.count > 0) {
            const double dt = a_clock - r.s[r.head].t;
            if (dt < 0.0 || dt > kTrendMaxGap) r.count = 0;       // restart (revert / long gap)
            else if (dt < kTrendMinStep)       return;            // decimate
        }
        r.head = (r.count == 0) ? 0 : (r.head + 1) % kTrendRing;
        r.s[r.head] = { a_clock, a_hpPct };
        r.count = std::min(r.count + 1, kTrendRing);
    }

    // Net HP fraction lost per second over the read window (>= 0; 0 = holding,
    // healing, or not enough span yet).
    inline float HpLossRate(RE::FormID a_id) {
        std::lock_guard lk(detail::g_trendLock);
        const auto it = detail::g_trend.find(a_id);
        if (it == detail::g_trend.end() || it->second.count < 2) return 0.0f;
        const auto& r   = it->second;
        const auto& nw  = r.s[r.head];
        const auto& nw1 = r.s[(r.head + kTrendRing - 1) % kTrendRing];
        const double window = std::max(kTrendWindow, 2.5 * (nw.t - nw1.t));
        const detail::HpSample* old = &nw1;
        for (std::size_t k = 2; k < r.count; ++k) {
            const auto& c = r.s[(r.head + kTrendRing - k) % kTrendRing];
            if (nw.t - c.t > window) break;
            old = &c;
        }
        const double span = nw.t - old->t;
        if (span < kTrendMinSpan) return 0.0f;
        return std::max(0.0f, static_cast<float>((old->hp - nw.hp) / span));
    }

    // The trend factor for a loss rate a_rate (/s) at health a_hpPct: time to
    // death over kTrendTtdSafe, in [kTrendFloor, 1].
    inline float TrendFactor(float a_hpPct, float a_rate) {
        if (a_rate <= kTrendRateEps) return 1.0f;
        return std::clamp((a_hpPct / a_rate) / kTrendTtdSafe, kTrendFloor, 1.0f);
    }
    inline float TrendFactor(RE::Actor* a_follower) {
        if (!a_follower) return 1.0f;
        return TrendFactor(Vocab::HealthPct(a_follower), HpLossRate(a_follower->GetFormID()));
    }

    // Revert / load (Scheduler::ClearTransientState): the ring is session state.
    inline void ClearTrend() {
        std::lock_guard lk(detail::g_trendLock);
        detail::g_trend.clear();
    }

    // [0,1]: how safe a_follower feels operating alone -- vitality, times the HP
    // trend, knocked down in a live fight by the weighted foe load (see the v2
    // block above). Must NOT be called with a combat-group lock held (FoeLoad
    // takes it; the Evaluator reads ChaseRadius before its own group scan).
    inline float Of(RE::Actor* a_follower) {
        if (!a_follower) return 0.0f;
        float c = Vitality(a_follower) * TrendFactor(a_follower);
        if (a_follower->IsInCombat()) {
            const float load = CombatSense::FoeLoad(a_follower);
            c *= FoeMultiplier(load > 0.0f ? load : 1.0f);   // empty group in combat = one even foe (v1's max(1))
        }
        return std::clamp(c, 0.0f, 1.0f);
    }

    // ESTIMATE, out of combat: the Of() a_follower would read once fighting a_foes foes
    // (at least one), each counted as an EVEN foe (weight 1.0 -- the probe makes no
    // strength read) -- his vitality times his current trend times the same fight
    // multiplier. Makes no engine combat read. The engage-on-sight gate
    // (EngageOnSight.cpp) compares it with the retreat floor, so a follower never
    // starts a fight he would at once retreat from.
    inline float OfFacing(RE::Actor* a_follower, int a_foes) {
        if (!a_follower) return 0.0f;
        return std::clamp(Vitality(a_follower) * TrendFactor(a_follower) *
                              FoeMultiplier(static_cast<float>(std::max(1, a_foes))),
                          0.0f, 1.0f);
    }

    // The confidence-scaled leash from the player, in game units: lerp between
    // the scared floor and the confident ceiling (Config, MCM-tunable).
    inline float LeashRadius(RE::Actor* a_follower) {
        const float lo = Config::g_leashMin.load();
        const float hi = Config::g_leashMax.load();
        return lo + Of(a_follower) * std::max(0.0f, hi - lo);
    }

    // THE SECOND CONSUMER this header always promised (#22): the confidence-
    // scaled COMBAT chase radius, in game units, measured from the FOLLOWER. How
    // far out he will pick a foe to engage. Hurt/mobbed -> low -> he fights only
    // what is on top of him and holds near the player; healthy/safe -> high ->
    // he ranges across the field. Gates PickFoe so "attack the weakest foe"
    // (distance-blind) can't send him charging THROUGH a pack to a distant
    // target -- Erik's 20-Falmer suicide charge. The floor stays well above
    // melee reach so he always defends himself against an adjacent foe.
    inline float ChaseRadius(RE::Actor* a_follower) {
        const float lo = Config::g_chaseMin.load();
        const float hi = Config::g_chaseMax.load();
        return lo + Of(a_follower) * std::max(0.0f, hi - lo);
    }
}
