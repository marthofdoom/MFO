// progression/Remove.cpp -- ROSTER REMOVAL, the progression half (ClickUp 86e3eewaf,
// marth 2026-09-24: "Removal leaves the NPC as it was before MFO").
//
// RemoveFollowerRecord undoes every actor write progression makes, then erases the
// follower's g_prog record. The caller (roster/Remove.cpp) has already run the
// dismissal release road and holds the worker pump paused. MAIN THREAD (g_prog).
//
// What progression writes to an actor, and how each is undone here:
//   * granted perk ranks: TESNPC::AddPerk in GrantRank / ReapplyFollower (PerkGate.cpp)
//     -> the rank form each PerkAlloc holds comes off the base (Respec's own loop,
//     Verbs.cpp), ApplyPerksFromBase once.
//   * stripped native perks: TESNPC::RemovePerk in StripNativePerks (PerkGate.cpp),
//     recorded in ProgState::strippedPerks -> RestoreNativePerksImpl, AFTER the
//     grants come off: a form can be BOTH (native rank 2 stripped while MFO held
//     rank 1, then MFO granted rank 2); restored first it would read as already
//     present and the grant loop would then take the native off.
//   * skill base AVs: SetBaseActorValue in ReconcileSkill (SkillScale.cpp, the single
//     skill write site) -> UnwindSkills settles every entry with zero points through
//     that same site.
//   * base H/M/S: SetFollowerHMS in RecomputeHMS (Hms.cpp; the split, parity hold and
//     fixed-stat grant all land there) -> each pool goes back to hmsBaseline, the
//     base captured before MFO held anything. A fixed-stat NPC is then exact; a
//     leveling NPC sits at that base until the engine's next absolute autocalc
//     slam on its next level, which is what it would get without MFO.
// Base perk edits never survive a load (P3), so the perk half also self-heals on the
// next load; the AV half is saved into the .ess and is only undone here.
#include "PCH.h"
#include <cmath>     // std::isfinite
#include "ProgAllocator.h"
#include "ProgAllocator_internal.h"
#include "Followers.h"   // Get/SetFollowerHMS (the canonical H/M/S base access)
#include <mutex>
#include <spdlog/spdlog.h>

namespace MFO::ProgAllocator {

    RemovalResult RemoveFollowerRecord(RE::Actor* a_actor) {
        RemovalResult r;
        if (!a_actor) return r;
        const auto id = a_actor->GetFormID();
        // The off-class fired mask is a runtime mirror the worker writes; drop it
        // whether or not a record exists (a never-enrolled follower can have one).
        { std::scoped_lock fl(g_hmsFireMx); g_hmsFiredMask.erase(id); }

        auto it = g_prog.find(id);
        if (it == g_prog.end()) return r;
        r.hadRecord = true;
        auto& st    = it->second;
        r.enrolled  = st.enrolled;
        auto* base  = a_actor->GetActorBase();

        // (1) MFO's granted ranks off the BASE -- the same rank-form resolution as
        // Respec (Verbs.cpp), counted and read back.
        if (!st.perks.empty()) {
            if (!base) {
                r.perkFails += static_cast<int>(st.perks.size());
                spdlog::warn("[roster] {:08X}: no actor base -- {} granted perk(s) NOT removed",
                             id, st.perks.size());
            } else {
                bool changed = false;
                for (const auto& p : st.perks) {
                    auto ref = FindNode(p.nodePerkID);
                    RE::BGSPerk* held = nullptr;
                    if (ref.node && p.rank >= 1 && p.rank <= ref.node->ranks.size())
                        held = PerkByID(ref.node->ranks[p.rank - 1].perkFormID);
                    if (!held) held = PerkByID(p.nodePerkID);   // off-catalog: Respec's best effort
                    if (!held) {
                        ++r.perkFails;
                        spdlog::warn("[roster] {:08X}: granted perk node {:08X} rank {} does not resolve "
                                     "-- NOT removed", id, p.nodePerkID, p.rank);
                        continue;
                    }
                    if (!base->GetPerkIndex(held).has_value()) { ++r.perksAbsent; continue; }
                    base->RemovePerk(held);
                    changed = true;
                    if (base->GetPerkIndex(held).has_value()) {
                        ++r.perkFails;
                        spdlog::warn("[roster] {:08X}: perk '{}' ({:08X}) still on the base after RemovePerk",
                                     id, NameOf(held), held->GetFormID());
                    } else {
                        ++r.perksRemoved;
                    }
                }
                if (changed) a_actor->ApplyPerksFromBase();
            }
        }

        // (2) the stripped natives back (enrolled records only ever strip).
        if (!st.strippedPerks.empty()) {
            for (const auto fid : st.strippedPerks) {
                if (!PerkByID(fid)) {
                    ++r.nativeFails;
                    spdlog::warn("[roster] {:08X}: stripped native perk {:08X} no longer resolves -- "
                                 "NOT restored (a load puts natives back anyway)", id, fid);
                }
            }
            if (!base) {
                r.nativeFails = static_cast<int>(st.strippedPerks.size());
                spdlog::warn("[roster] {:08X}: no actor base -- {} stripped native perk(s) NOT restored",
                             id, st.strippedPerks.size());
            } else {
                r.nativesRestored = RestoreNativePerksImpl(a_actor, base, st);
            }
        }

        // (3) every skill back to natural through the single write site.
        if (!st.skills.empty())
            r.skillsReset = UnwindSkills(a_actor, st, r.skillFails);

        // (4) base H/M/S back to the captured pre-MFO baseline. An uncaptured block
        // never held anything (the first RecomputeHMS captures before it writes).
        if (st.hmsCaptured) {
            float cur[3];
            for (int p = 0; p < 3; ++p) cur[p] = Followers::GetFollowerHMS(a_actor, p);
            for (int p = 0; p < 3; ++p) {
                const float tgt = st.hmsBaseline[p];
                // Health 0 would kill him; a negative or non-finite base is corrupt.
                if (!std::isfinite(tgt) || tgt < 0.0f || (p == 0 && tgt <= 0.0f)) {
                    ++r.hmsFails;
                    spdlog::warn("[roster] {:08X}: HMS pool {} baseline {:.1f} is not usable -- base left at {:.1f}",
                                 id, p, tgt, cur[p]);
                    continue;
                }
                if (tgt == cur[p]) continue;
                Followers::SetFollowerHMS(a_actor, p, tgt);
                const float now = Followers::GetFollowerHMS(a_actor, p);
                if (now != tgt) {
                    ++r.hmsFails;
                    spdlog::warn("[roster] {:08X}: HMS pool {} write did NOT stick (wrote {:.1f}, reads {:.1f})",
                                 id, p, tgt, now);
                } else {
                    ++r.hmsPoolsReset;
                    spdlog::info("[roster] {:08X}: HMS pool {} base {:.1f} -> baseline {:.1f}", id, p, cur[p], tgt);
                }
            }
            // HEALTH GUARD, RecomputeHMS's shape: when base Health went DOWN, heal at
            // most the drop and never more than the damage held, so current health
            // does not fall with the base.
            const float newH = Followers::GetFollowerHMS(a_actor, 0);
            if (newH < cur[0]) {
                const float drop = cur[0] - newH;
                const float dmg  = std::max(0.0f, -a_actor->GetActorValueModifier(
                                       RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth));
                const float heal = std::min(drop, dmg);
                if (auto* avo = a_actor->AsActorValueOwner(); avo && heal > 0.0f) {
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                                                    RE::ActorValue::kHealth, heal);
                    r.healed = heal;
                }
            }
        }

        g_prog.erase(it);
        PublishBoardViews();
        return r;
    }

}
