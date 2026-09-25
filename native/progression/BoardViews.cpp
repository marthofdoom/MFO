// progression/BoardViews.cpp -- COMPONENT 3, the board views: the published
// snapshot and its focus/refresh state, EnrollBlocker, BuildNodeViews and the
// public Copy/Publish/SetBoardFocus entry points.
// Split out of the old native/ProgAllocator.cpp by the wave-1 subsystem-folder
// split (2026-09-24): a pure move, proven function by function with tools/splitcheck.
// The allocator's contract and design references are at the top of
// progression/Allocator.cpp.
#include "PCH.h"
#include <cmath>     // std::floor / std::isfinite — not guaranteed via the PCH
#include "ProgAllocator.h"
#include "ProgAllocator_internal.h"   // the split family's shared substrate
#include "Progression.h"
#include "Forms.h"   // §18.6: the addon-manifest sentinel keyword
#include "Board.h"
#include "Config.h"
#include "Followers.h"
#include "Rapport.h"
#include "MainThread.h"
#include "Serialization.h"
#include "State.h"

namespace MFO::ProgAllocator {

        // ── board views (component 3 — see ProgAllocator.h) ─────────────────
        // Written on the MAIN thread only; the mutex guards the shared_ptr
        // swap so any thread may take a refcounted copy. The snap itself is
        // immutable once published — the Snapshot discipline, by pointer.
        std::mutex g_viewMx;
        std::shared_ptr<const BoardProgSnap> g_boardSnap;
        std::atomic<RE::FormID> g_boardFocus{ 0 };   // written by the render thread
        RE::FormID g_lastPublishedFocus = 0;         // main-thread-only
        bool g_boardWasOpen = false;                 // main-thread-only
        int  g_viewFrames   = 0;

    namespace {

        // Read-only mirror of Enroll()'s refusals, for the board to explain
        // an ineligible follower BEFORE the player tries (same checks, same
        // order — keep in sync with Enroll).
        const char* EnrollBlocker(RE::Actor* a_actor, RE::TESNPC* a_base) {
            if (!a_base) return "no actor base";
            // ASCII only: these strings render in the board UI and the baked
            // fonts carry the default (Basic Latin) glyph ranges.
            if (!Followers::IsPersistableID(a_actor->GetFormID()))
                return "temporary (runtime) actor -- cannot persist across saves";
            if (!a_base->IsUnique())
                return "shared template -- not eligible (v1 is unique-base only)";
            if (!Followers::IsEligibleFollower(a_actor))
                return "not an eligible follower";
            return nullptr;
        }

        // One BoardNodeView per catalog node, catalog order (skill-major) —
        // the render thread indexes by prefix sums off the same frozen
        // catalog. Cost is bounded by the publish cadence (kViewFrames), not
        // per frame: GateNextRank's condition evaluation only runs for nodes
        // that survive the cheaper gates (its points check sits before the
        // prereq walk and perkConditions.IsTrue).
        void BuildNodeViews(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState* a_st,
                            std::vector<BoardNodeView>& a_out) {
            static const ProgState kNoState{};
            const auto& cat = Progression::Get();
            std::size_t total = 0;
            for (const auto& t : cat.skills) total += t.nodes.size();
            a_out.clear();
            a_out.reserve(total);
            const bool gateable = a_st && a_st->enrolled && FindClassDef(a_st->clsId) != nullptr;
            for (const auto& tree : cat.skills) {
                for (const auto& node : tree.nodes) {
                    BoardNodeView v;
                    if (a_st)
                        if (auto* al = FindAlloc(*a_st, node.perkFormID))
                            v.ownedRank = al->rank;
                    if (v.ownedRank == 0 &&
                        OwnsAnyRank(a_actor, a_base, a_st ? *a_st : kNoState, node.perkFormID))
                        v.native = true;   // load-order-granted — MFO never builds on it (§4.1)
                    if (gateable && !v.native &&
                        v.ownedRank < static_cast<std::uint8_t>(node.ranks.size())) {
                        std::string why;
                        if (GateNextRank(a_actor, a_base, *a_st, node, why) > 0)
                            v.available = true;
                        else
                            v.whyNot = std::move(why);
                    }
                    a_out.push_back(std::move(v));
                }
            }
        }

    }

    // ── board views (component 3) ───────────────────────────────────────────

    void SetBoardFocus(RE::FormID a_id) { g_boardFocus.store(a_id); }

    std::shared_ptr<const BoardProgSnap> CopyBoardViews() {
        std::scoped_lock lk(g_viewMx);
        return g_boardSnap;
    }

    // v1.1 Phase 6b: wrap the published payload into the GENERIC hosted-tab list.
    // One BoardTabView per add-on that declared a board tab (frozen Manifests()),
    // carrying the add-on's own label + the shared payload; active iff the payload
    // is published+ready. Delete the add-on → Manifests() empty → no hosted tab.
    std::vector<BoardTabView> CopyBoardTabViews() {
        std::vector<BoardTabView> tabs;
        auto snap = CopyBoardViews();
        for (const auto& m : Progression::Manifests()) {
            if (!m.boardTab.declared) continue;
            BoardTabView v;
            v.label   = m.boardTab.label;
            v.content = snap;
            v.active  = (snap && snap->active);
            tabs.push_back(std::move(v));
        }
        return tabs;
    }

    void PublishBoardViews() {
        auto snap = std::make_shared<BoardProgSnap>();
        snap->active = g_ready && Progression::Get().built;
        if (snap->active) {
            snap->respecRapportCost     = g_econ.respecRapportCost;
            snap->skillCap              = g_econ.skillCap;
            snap->levelsPerPerkPoint    = std::max(1, g_econ.levelsPerPerkPoint);
            snap->manualSkillPtsPerLevel = g_econ.manualSkillPtsPerLevel;
            // §18.6: the declared classes, in declaration order — the board's
            // dynamic-N class prompt draws exactly these.
            snap->classes.reserve(g_classes.size());
            for (const auto& def : g_classes)
                snap->classes.emplace_back(def.id, def.name);

            const RE::FormID focus = g_boardFocus.load();
            // Immutable snapshot, not live g_active: Refresh reassigns it on the
            // worker while this poll runs on the MainThread::Post chain (SEV-1).
            auto activeSnap = Followers::ActiveSnapshot();
            for (RE::FormID snapId : *activeSnap) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(snapId);
                if (!a) continue;
                BoardFollowerView v;
                v.id   = a->GetFormID();
                v.name = a->GetName() ? a->GetName() : "?";
                auto* base = a->GetActorBase();
                const char* blk = EnrollBlocker(a, base);
                v.eligible = (blk == nullptr);
                if (blk) v.blocker = blk;
                ProgState* st = nullptr;
                if (auto it = g_prog.find(v.id); it != g_prog.end() && it->second.enrolled)
                    st = &it->second;
                if (st) {
                    v.enrolled       = true;
                    v.clsId          = st->clsId;
                    if (const auto* def = FindClassDef(st->clsId)) v.clsName = def->name;
                    v.level          = st->progressionLevel;
                    v.unspentPerk    = static_cast<float>(PerkPointsAvailable(*st));   // §17 derived
                    v.nativeAtEnroll = st->nativeTreePerksAtEnroll;
                    v.allocatedRanks = static_cast<std::uint16_t>(
                        std::min(AllocatedRanks(*st), 0xFFFF));
                    v.manualSkills   = st->manualSkills;           // §16
                    v.manualAvail    = ManualAvail(*st);
                    v.freeRespec     = st->freeRespec;             // one-time free respec pending
                }
                if (auto* avo = a->AsActorValueOwner()) {
                    v.skills.reserve(std::size(kSkillNames));
                    for (const auto& s : kSkillNames) {
                        BoardSkillLine line;
                        line.name = s.name;
                        line.av   = s.av;
                        line.base = avo->GetBaseActorValue(s.av);
                        if (st)
                            for (const auto& e : st->skills)
                                if (e.av == s.av) {
                                    line.alloc  = e.points;
                                    line.manual = e.manualPoints;   // §16
                                    break;
                                }
                        v.skills.push_back(std::move(line));
                    }
                }
                // The full per-node gate walk only for the follower the tab
                // is actually LOOKING at — one tree per publish, not four.
                if (base && v.id == focus) {
                    snap->treeFor = focus;
                    BuildNodeViews(a, base, st, snap->nodes);
                }
                snap->rows.push_back(std::move(v));
            }
            g_lastPublishedFocus = focus;
        }
        std::scoped_lock lk(g_viewMx);
        g_boardSnap = std::move(snap);
    }

}
