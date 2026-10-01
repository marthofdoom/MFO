// roster/Remove.cpp -- "Remove from roster" (ClickUp 86e3eewaf). See roster/Roster.h.
//
// THE SPEC'S ORDER, AND WHICH THREAD RUNS EACH STEP
//   0. Board::ApplyEdits drains a RemoveFromRoster edit on the serial task WORKER
//      (the domain that owns g_followers, #4 / #74) and calls RequestRemove.
//   1. RELEASE (worker): Followers::ReleaseHeldState, the dismissal road itself --
//      equip authority + declaration, Loadout restore, target / cast / stance
//      latches, the force-hold (FWPN entry erased there), the three serialized alias
//      fills, every APMF claim. Idempotent: a dismissed follower already ran it.
//   2. UNDO + 3. ERASE (true MAIN thread, MainThread::Post): g_prog is main-thread
//      state and every actor write progression made is a main-thread write, so this
//      phase cannot run on the worker. It holds the worker pump PAUSED for its whole
//      body (Diagnostics::PausePump / ResumePump, RAII) -- the SaveCallback pattern:
//      PausePump makes every PumpTickGate'd AddTask body bail and drains the one that
//      may already be running, so no worker body touches g_followers (or the
//      Followers maps, or Logistics' worker state) while this erases from them. Main
//      and the worker are not proven mutually exclusive (#74); the pause is what
//      makes the erase safe, not the phasing. Nothing here INSERTS into g_followers.
//      Under the pause it re-checks that he still has a record and is still not a
//      follower (he could have rejoined between the click and this frame), then:
//      ProgAllocator::RemoveFollowerRecord (perks, natives, skills, HMS; g_prog
//      erased), Followers::EraseRecord (FLWR + mirrors), Logistics::EraseStockGear
//      (MSTK), PlayerGiven::ForgetFollower (PGIV). The next save simply omits him:
//      no record layout or version changes, readers untouched.
//   A revert / load between the post and the run bumps ProgAllocator::PollGeneration
//   and the body bails (the AddonAction edit's guard), so a stale removal can never
//   land on the next save's same-FormID actor.
#include "PCH.h"
#include "Roster.h"
#include "State.h"
#include "Followers.h"
#include "Diagnostics.h"              // PausePump / ResumePump
#include "MainThread.h"
#include "progression/ProgAllocator.h"
#include "logistics/Logistics.h"      // EraseStockGear (MSTK)
#include "logistics/PlayerGiven.h"    // ForgetFollower (PGIV)
#include <string>
#include <spdlog/spdlog.h>

namespace MFO::Roster {

    namespace {

        void RemoveOnMain(RE::FormID a_id, int a_gen, const std::string& a_name) {
            if (ProgAllocator::PollGeneration() != a_gen) {
                spdlog::info("[roster] {:08X} {} removal dropped: superseded by revert/reload", a_id, a_name);
                return;
            }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
            if (!actor) {
                spdlog::warn("[roster] {:08X} {} removal REFUSED on main: actor no longer resolves -- nothing "
                             "undone, nothing erased", a_id, a_name);
                return;
            }

            Diagnostics::PausePump();
            struct ResumeGuard { ~ResumeGuard() { Diagnostics::ResumePump(); } } resume;

            if (g_followers.find(a_id) == g_followers.end()) {
                spdlog::info("[roster] {:08X} {} removal: no record left (already removed)", a_id, a_name);
                return;
            }
            // Re-check under the pause: the mirror Refresh publishes (#74's off-worker
            // road) and the engine's own teammate state. A follower back in the party
            // would be re-adopted next Refresh with default data -- refuse instead.
            if (Followers::IsTrackedFast(a_id) || Followers::IsEligibleFollower(actor)) {
                spdlog::warn("[roster] {:08X} {} removal REFUSED: following again -- nothing undone, nothing erased "
                             "(the release already ran; MFO re-claims him on its next tick)", a_id, a_name);
                return;
            }

            // 2. UNDO what progression wrote to the actor, then erase g_prog.
            const auto pr = ProgAllocator::RemoveFollowerRecord(actor);
            // 3. ERASE every other record keyed by him.
            const bool        flwr = Followers::EraseRecord(a_id);
            const std::size_t mstk = Logistics::EraseStockGear(a_id);
            const std::size_t pgiv = PlayerGiven::ForgetFollower(a_id);

            const int fails = pr.perkFails + pr.nativeFails + pr.skillFails + pr.hmsFails;
            spdlog::info("[roster] {:08X} {} removed: release=done perks-removed={} perks-absent={} "
                         "natives-restored={} skills-reset={} hms-pools-reset={} healed={:.1f} "
                         "records: flwr={} prgn={}{} mstk={} pgiv={} failures={}",
                         a_id, a_name, pr.perksRemoved, pr.perksAbsent, pr.nativesRestored,
                         pr.skillsReset, pr.hmsPoolsReset, pr.healed,
                         flwr ? 1 : 0, pr.hadRecord ? 1 : 0,
                         pr.hadRecord ? (pr.enrolled ? "(enrolled)" : "(hms-only)") : "",
                         mstk, pgiv, fails);
            if (fails > 0)
                spdlog::warn("[roster] {:08X} {} removal left {} change(s) NOT undone: perks={} natives={} "
                             "skills={} hms={} (each named in a [roster] warn line above)",
                             a_id, a_name, fails, pr.perkFails, pr.nativeFails, pr.skillFails, pr.hmsFails);
        }

    }

    void RequestRemove(RE::FormID a_id) {
        if (g_followers.find(a_id) == g_followers.end()) {
            spdlog::warn("[roster] {:08X} removal REFUSED: no MFO record", a_id);
            return;
        }
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
        if (!actor) {
            spdlog::warn("[roster] {:08X} removal REFUSED: actor does not resolve -- nothing touched", a_id);
            return;
        }
        std::string name = actor->GetName() ? actor->GetName() : "?";
        if (Followers::IsTracked(a_id)) {
            spdlog::warn("[roster] {:08X} {} removal REFUSED: currently following (dismiss first, or uncheck "
                         "MFO to keep them but have MFO leave them alone)", a_id, name);
            return;
        }
        if (!MainThread::IsInstalled()) {
            // The undo needs the true main thread; never run it here instead.
            spdlog::error("[roster] {:08X} {} removal REFUSED: the main-thread pump is not installed", a_id, name);
            return;
        }
        spdlog::info("[roster] {:08X} {} removal requested -- releasing held state (the dismissal road)", a_id, name);
        // 1. RELEASE, on this worker, exactly as dismissal does.
        Followers::ReleaseHeldState(a_id);
        const int gen = ProgAllocator::PollGeneration();
        MainThread::Post([a_id, gen, name = std::move(name)]() { RemoveOnMain(a_id, gen, name); });
    }

}
