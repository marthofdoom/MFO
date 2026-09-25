// progression/Harness.cpp -- THE DEV HARNESS (bProgHarness): OnHarnessHotkey
// and its status / skill / economy dumps, plus ClsName (class name by id).
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

        // Class display name by id — "none" for 0/unknown (log convenience).
        const char* ClsName(RE::FormID a_id) {
            const auto* def = FindClassDef(a_id);
            return def ? def->name.c_str() : "none";
        }

    namespace {

        // ── the dev harness (bProgHarness) ──────────────────────────────────

        // First unique-base persistable teammate, else first — the ProgProbe
        // pick, without its per-candidate spam (the harness runs repeatedly).
        RE::Actor* PickFollower() {
            RE::Actor* first = nullptr;
            RE::Actor* firstUnique = nullptr;
            // Iterate the immutable snapshot, not live g_active (Refresh rebuilds
            // it on the worker -> UAF for this off-worker poll). Resolve each id
            // on the form table (main-thread safe here).
            auto snap = Followers::ActiveSnapshot();
            for (RE::FormID id : *snap) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!a) continue;
                if (!first) first = a;
                auto* base = a->GetActorBase();
                if (!firstUnique && base && base->IsUnique() &&
                    Followers::IsPersistableID(a->GetFormID()))
                    firstUnique = a;
            }
            if (!first) spdlog::info("[prog] harness: no active teammates — recruit a follower first");
            return firstUnique ? firstUnique : first;
        }

        void HarnessStatus(RE::Actor* a_actor) {
            const auto id = a_actor->GetFormID();
            auto it = g_prog.find(id);
            if (it == g_prog.end() || !it->second.enrolled) {
                spdlog::info("[prog] status {}: NOT ENROLLED (cmd 1 enrolls; player level {})",
                             NameOf(a_actor),
                             RE::PlayerCharacter::GetSingleton()
                                 ? RE::PlayerCharacter::GetSingleton()->GetLevel() : 0);
                return;
            }
            auto& st = it->second;
            spdlog::info("[prog] status {} ({:08X}): class {} | progression level {} | "
                         "{} perk point(s) available (§17: floor(lvl/{}) − {} spent) | "
                         "{} perk(s) allocated | {} | "
                         "sharedRemainder {} | level-matched {} | provenance PFF={}",
                         NameOf(a_actor), id, ClsName(st.clsId), st.progressionLevel,
                         PerkPointsAvailable(st), g_econ.levelsPerPerkPoint,
                         AllocatedRanks(st), st.perks.size(),
                         IsActiveFollower(id) ? "ACTIVE" : "benched",
                         st.sharedGrowthRemainder,
                         st.veteranConsumed ? "yes" : "pending",
                         st.wasInPotentialFollowerFaction ? "yes" : "no");
            for (const auto& p : st.perks) {
                auto ref = FindNode(p.nodePerkID);
                spdlog::info("[prog]   perk {} ({:08X}) rank {}",
                             ref.node ? ref.node->name.c_str() : "<off-catalog>",
                             p.nodePerkID, p.rank);
            }
        }

        void HarnessSkillDump(RE::Actor* a_actor) {
            auto it = g_prog.find(a_actor->GetFormID());
            if (it == g_prog.end() || !it->second.enrolled || it->second.clsId == 0) {
                spdlog::info("[prog] skills {}: no class set — nothing auto-scaled", NameOf(a_actor));
                return;
            }
            auto* avo = a_actor->AsActorValueOwner();
            if (!avo) return;
            spdlog::info("[prog] skills {} (class {}, level {}):", NameOf(a_actor),
                         ClsName(it->second.clsId), it->second.progressionLevel);
            for (const auto& e : it->second.skills) {
                spdlog::info("[prog]   {:<12} natural {:.1f} + alloc {:.0f} = base {:.1f} "
                             "(effective {:.1f})",
                             AvName(e.av), e.lastWrittenBase - e.points, e.points,
                             avo->GetBaseActorValue(e.av), avo->GetActorValue(e.av));
            }
            if (it->second.skills.empty())
                spdlog::info("[prog]   (no allocations — level too low for any points yet)");
        }

        void HarnessEconomyDump() {
            const auto& c = Progression::Get();
            spdlog::info("[prog] economy: perk = 1 per {} level(s) (§17 derived, minus "
                         "spent) | skill/lvl {:g} | manual skill/lvl {} | "
                         "sharedDiv {} | respec rapport {:g} | cap {:g} | catalog ranks {}/{} | "
                         "lastPlayerLevel {}",
                         g_econ.levelsPerPerkPoint, g_econ.skillPointsPerLevel,
                         g_econ.manualSkillPtsPerLevel, g_econ.sharedGrowthDivisor,
                         g_econ.respecRapportCost, g_econ.skillCap, c.effectiveRanks,
                         c.totalRanks, g_lastPlayerLevel);
        }

    }

    // ── dev harness ─────────────────────────────────────────────────────────

    void OnHarnessHotkey() {
        if (!Config::g_progHarness.load()) return;
        if (!g_ready) {
            spdlog::info("[prog] harness: addon absent — nothing to exercise");
            return;
        }
        const int cmd = g_devCmd ? static_cast<int>(g_devCmd->value) : 0;
        auto* f = PickFollower();
        if (!f) return;   // logged by the pick
        spdlog::info("[prog] ===== HARNESS cmd {} on {} ({:08X}) =====",
                     cmd, NameOf(f), f->GetFormID());
        switch (cmd) {
        case 0:
            HarnessStatus(f);
            spdlog::info("[prog] harness cmds (console `set MFOP_DevCmd to N`): 0=status "
                         "1=enroll 2=cycle-class 3=skills 4=alloc-next-perk 5=respec 6=economy");
            break;
        case 1:
            Enroll(f);
            break;
        case 2: {
            auto it = g_prog.find(f->GetFormID());
            if (it == g_prog.end() || !it->second.enrolled) {
                spdlog::info("[prog] harness: {} not enrolled — cmd 1 first", NameOf(f));
                break;
            }
            if (g_classes.empty()) {
                spdlog::info("[prog] harness: no classes declared by any addon");
                break;
            }
            // Cycle through the DECLARED classes (§18.6 dynamic-N).
            std::size_t idx = 0;
            for (std::size_t k = 0; k < g_classes.size(); ++k)
                if (g_classes[k].id == it->second.clsId) { idx = k + 1; break; }
            SetClass(f, g_classes[idx % g_classes.size()].id);
            break;
        }
        case 3:
            HarnessSkillDump(f);
            break;
        case 4:
            AllocateNextEligible(f);
            break;
        case 5:
            Respec(f);
            break;
        case 6:
            HarnessEconomyDump();
            break;
        default:
            spdlog::info("[prog] harness: unknown cmd {} (0..6)", cmd);
            break;
        }
    }

}
