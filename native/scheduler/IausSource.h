#pragma once
// THE IAUS CHOICE SOURCE (IAUS card I6, design Docs/research/2026-11/iaus-design.md 1.1-1.4; research note
// Docs/research/2026-11/iaus-i6.md). The idea of IAUS for MFO is LandingCrew's (iaus/Core.h).
//
// The second ChoiceSource behind the I1 seam. For a follower whose FLWR bit 3 (FollowerState::iausCombat, card
// I5) is set, the combat scan asks THIS source for its choices instead of the gambit table. It replaces only the
// SOURCE of the choices: the scan loop, Actuation::Fire, every road, claim, guard and animation rule, the
// reconciliation and the [eval] lines are the table's own, unchanged (composition, not substitution).
//
// ONE LAP: the constructor gathers and decides (Candidates::Build, Engine::FillCooldowns, Engine::Decide,
// DecidePosture, Candidates::NoteDecision, in that order). The walk it then hands the scan is:
//   1. THE POSTURE LAYER's winner as an act.equip_melee / act.equip_ranged / act.equip_silver choice (design 1.2).
//      A satisfied equip is transparent, so the scan claims the hand (handClaim / wantStance / equipHeld /
//      silverHeld, its own writes) and goes on. Rank = Iaus::kRankBase: a stable identity that outranks NO action
//      decision, so a weapon swap never preempts a standing heal's hand.
//   2. THE ACTION LAYER: Decision::order, one candidate per Next, at most Tuning::maxTries. A transparent outcome
//      moves to the next one, exactly like a transparent rule.
// The commitment window is INSIDE Decide (Verdict::Hold), so SuppressionAnchor is always "no window" here and the
// table's positional window never runs for an IAUS follower.
//
// NO ROW IS EVER READ: an IAUS rank is never a table index (scheduler/Rank.h).
//
// THREADING: the g_followers worker domain (#4), like the table source. Built per follower per lap inside
// Scheduler::Tick, never outlives it. The Candidates::Set it owns is neither copied nor moved (candidate names
// are string_views into its snapshot), which is why the source itself is not copyable.

#include "PCH.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "iaus/Candidates.h"
#include "iaus/Engine.h"
#include "scheduler/ChoiceSource.h"

namespace MFO::Scheduler {

    class IausSource final : public ChoiceSource {
    public:
        IausSource(RE::Actor* a_self, RE::FormID a_id, std::uint8_t a_rapportRank);
        ~IausSource() override;   // the [iaus] line, the generation line, the board readout
        IausSource(const IausSource&) = delete;
        IausSource& operator=(const IausSource&) = delete;

        [[nodiscard]] Eval::Source Kind() const override { return Eval::Source::Iaus; }
        bool Next(Eval::Choice& a_out) override;
        void Reject(const Eval::Choice& a_choice, std::string_view a_why) override;
        [[nodiscard]] int SuppressionAnchor(bool, int, std::uint32_t) const override { return Rank::kNone; }
        [[nodiscard]] std::uint32_t FiredIdentity(const Eval::Choice& a_choice) const override;
        [[nodiscard]] std::string CoolingReason(const Eval::Choice&, std::chrono::steady_clock::time_point) const override {
            return {};   // an IAUS hard cooldown is a gate inside the scorer (Cons::CooldownHard), never a passed-over row
        }
        [[nodiscard]] bool HesitationExempt(const Eval::Choice& a_choice) const override;
        void NoteSkipReason(const Eval::Choice&, const std::string&) override {}   // Reject carries every reason
        void Record(const Eval::Choice& a_choice, const Actuation::Outcome& a_outcome,
                    std::chrono::steady_clock::time_point a_now) override;

        // RECONCILIATION reads: the posture layer's answer instead of the rows above the stop (design 1.2). Each
        // is asked only when the scan left equipHeld / silverHeld clear, so "the posture layer decided and its
        // order (if any) did not hold" is the table's "evaluated above the stop, did not hold".
        [[nodiscard]] bool EquipEvaluatedAboveStop(const Eval::Choice&) const override { return m_postureKnown; }
        [[nodiscard]] bool SilverEvaluatedAboveStop(const Eval::Choice&) const override { return m_postureKnown; }
        // The posture layer had NO ANSWER this lap (no fresh spell book, or no readable foe): the equip truth and
        // the silver truth are UNKNOWN, not false, and the holds are kept (the MFO-B55 shape).
        [[nodiscard]] bool UnholySilverEvaluated(const Eval::Choice&) const override { return !m_postureKnown; }
        [[nodiscard]] bool EquipRangeUndecidable(RE::Actor*, const Eval::Choice&) const override { return !m_postureKnown; }

        // Tick's IAUS branch, right after the scan. (1) Verdict::Hold that took no action is the "suppressed"
        // stop (the #26 analogue). (2) The RUNNING cast decision is still asked for while its candidate clears
        // the act floor, even on a lap the walk never reached it (design 1.3): the cast loan and HealSustainLap's
        // "the rule is still asking" both read it.
        void AmendScan(bool a_stopped, bool& a_suppressed, bool& a_castSeen, std::vector<int>& a_matchedCast) const;

    private:
        struct Item {
            bool        posture = false;
            std::size_t idx = 0;   // posture: into gen.postures; action: into Decision::order
        };
        [[nodiscard]] bool MakeChoice(const Item& a_item, Eval::Choice& a_out, std::string& a_whyNot) const;
        [[nodiscard]] const Item* Current() const { return m_pos < m_walk.size() ? &m_walk[m_pos] : nullptr; }

        RE::Actor*  m_self;
        RE::FormID  m_id;
        Iaus::Engine::Tick m_tick;
        Iaus::Candidates::Set m_set;
        Iaus::Decision m_d;
        double      m_ms = 0.0;
        bool        m_built = false;
        bool        m_postureKnown = false;   // the posture layer ran on a fresh book with a readable foe
        int         m_posture = -1;           // DecidePosture's winner (into gen.postures), -1 = no equip order wanted
        std::vector<Item> m_walk;
        std::size_t m_pos = 0;
        int         m_pickedStep = -1;        // the Decision::order index the scan stopped on with a Fired
        std::vector<Iaus::WalkNote> m_notes;  // transparent fall-pasts of action candidates, in walk order
        std::string m_postureNote;            // the posture choice's own fall-past ("satisfied", "hand claimed" ...)
    };

    // ── The lifecycle around the source (all WORKER, inside Scheduler::Tick) ─────────────────────────────────

    // THE TOGGLE EDGE (design 1.3, card I6 item 12). Call once per own service with the follower's effective
    // source (mfoEnabled && iausCombat). On a change, either direction, the running decision is STOOD DOWN: every
    // cast lock, heal hold, cast claim, cast-control latch, target latch and the suppression window of the old
    // source is released through the existing release calls, so no rank from the old source survives into the new
    // one. The forced weapon is NOT stripped (a mid-fight unequip is visible): the new source's own reconciliation
    // keeps or releases it through the normal dwell. Returns true on an edge.
    bool IausToggleEdge(RE::Actor* a_self, RE::FormID a_id, bool a_iausNow);

    // The party fight is over for a_id (the debounced party-OOC teardown).
    void IausCombatEnded(RE::FormID a_id);

    // Once per pump, before the per-follower early returns: forget the IAUS state of every follower that left
    // the party or lost his record (dismissal runs on main / Papyrus and may not touch the worker-only Minds:
    // MFO-B639). Cheap: one small map walk every few pumps.
    void IausSweep();

    // The retreat exception (marth 2026-10-01: "Heal while retreating? Themselves, yes. others no."): the IAUS
    // follower's best SELF heal through the package-free direct road, under the same three gates as the table's
    // retreat walk. No row is read.
    void IausRetreatSelfHeal(RE::Actor* a_self, RE::FormID a_id, std::uint8_t a_rapportRank,
                             std::chrono::steady_clock::time_point a_now);

}
