#pragma once
// THE CHOICE SEAM (IAUS card I1, design Docs/research/2026-11/iaus-design.md 1.1 and 1.3).
//
// The combat scan (scheduler/CombatScan.cpp) asks a ChoiceSource for its candidates instead of walking the
// gambit table itself. Everything that EXECUTES a choice stays in the scan (the hand claim, cannot-act, the
// cooldown and hesitation gates, Actuation::Fire, the reconciliation, the [eval] lines); the source only says
// which choices to try, in which order, and keeps its own bookkeeping.
//
// Today the only source is the table source (TableSource in CombatScan.cpp): the Evaluate(start) walk and the
// row reads the scan used to make on `ruleIndex`, moved behind these methods unchanged. Card I6 adds the IAUS
// source. A source is built per follower per lap on the worker (#4) and never outlives the lap.
//
// Every row read re-finds the follower's record by FormID (INVARIANTS #2): a transparent outcome may have
// dispatched engine events, so no source holds an iterator across a call.

#include "PCH.h"

#include <chrono>
#include <cstdint>
#include <string>

#include "Evaluator.h"
#include "cast/Actuation.h"
#include "scheduler/Rank.h"

namespace MFO::Scheduler {

    class ChoiceSource {
    public:
        virtual ~ChoiceSource() = default;

        [[nodiscard]] virtual Eval::Source Kind() const = 0;

        // The next candidate. false = the follower's record is gone: a_out is left UNTOUCHED (the scan keeps the
        // last choice it had, exactly as the pre-I1 loop's `break` did). true with a_out.rank < 0 = no more
        // candidates.
        virtual bool Next(Eval::Choice& a_out) = 0;

        // The scan passes over a_choice (a transparent outcome, the hand claim, cannot act): the next Next()
        // starts after it.
        virtual void Reject(const Eval::Choice& a_choice) = 0;

        // The positional suppression anchor (INVARIANTS #26, #11): the CURRENT rank of the decision that bought
        // the window, re-resolved from its stored identity. a_firedRank / a_firedIdentity are Recent::firedRule /
        // Recent::firedUID. -1 = the window is void.
        [[nodiscard]] virtual int SuppressionAnchor(bool a_inWindow, int a_firedRank,
                                                    std::uint32_t a_firedIdentity) const = 0;

        // The identity stored for a FIRED choice (Recent::firedUID; table: Gambit::uid, 0 = none).
        [[nodiscard]] virtual std::uint32_t FiredIdentity(const Eval::Choice& a_choice) const = 0;

        // Card G9: the "cooling down (Ns)" reason while a_choice is still cooling, else empty.
        [[nodiscard]] virtual std::string CoolingReason(const Eval::Choice& a_choice,
                                                        std::chrono::steady_clock::time_point a_now) const = 0;

        // Flair #5: a self-defence choice skips the retarget hesitation beat.
        [[nodiscard]] virtual bool HesitationExempt(const Eval::Choice& a_choice) const = 0;

        // G10 / G9: a passed-over choice's reason reaches the board.
        virtual void NoteSkipReason(const Eval::Choice& a_choice, const std::string& a_reason) = 0;

        // Bookkeeping for the choice the scan STOPPED on (Fired or an opaque hold): board display and the G9
        // cooldown start.
        virtual void Record(const Eval::Choice& a_choice, const Actuation::Outcome& a_outcome,
                            std::chrono::steady_clock::time_point a_now) = 0;

        // RECONCILIATION reads (T#76, silver, MFO-B55). a_stop is the choice the scan ended on (rank < 0 = the
        // walk ran out). "Evaluated" = the source tried it this lap above the stop and its condition did not
        // hold.
        [[nodiscard]] virtual bool EquipEvaluatedAboveStop(const Eval::Choice& a_stop) const = 0;
        [[nodiscard]] virtual bool SilverEvaluatedAboveStop(const Eval::Choice& a_stop) const = 0;
        [[nodiscard]] virtual bool UnholySilverEvaluated(const Eval::Choice& a_stop) const = 0;
        [[nodiscard]] virtual bool EquipRangeUndecidable(RE::Actor* a_self, const Eval::Choice& a_stop) const = 0;
    };

}
