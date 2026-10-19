// logistics/StuckAtOrigin.cpp -- a follower who is stuck at his OWN origin (field 2026-10-19, LoreRim Riften).
//
// WHAT HAPPENED. Sven was trapped for about 2 minutes at canal level. More than 13 DIFFERENT loot targets all
// ended BLOCKED at the same spot (a box at ~(172400..172680, -94135..-94300, z 10436..10527)). Each end was
// judged on its own: the target was GATED, the next one was tried, and it ended BLOCKED at the same place.
// The fault was never the targets, it was where HE stood. Thirteen gated targets that all block within a
// few hundred units of one point are one fact: this follower cannot leave this spot.
//
// THE RULE. Three or more consecutive static-block (GATED) ends, across DISTINCT targets, whose block points
// all lie within kStuckRadius of the newest one: he is stuck himself. The caller ends the excursion, releases
// the ch.19 road (Packages::LootTravelClear) and sends him back to follow; this file keeps the evidence and the
// SUPPRESSION (until he has moved > kStuckRadius from where he stood, or left the cell; no timer, no teleport) that stops the very next tick from dispatching another loot leg from the same box.
//
// THE RESCUE (marth 2026-10-19: "It should have him exit a door near his stuck position. but ONLY if hes really stuck.").
// An episode is "really stuck" only when EVERY criterion holds (decision logged with each value; a failing
// check is logged as a NEAR-MISS so the thresholds can be tuned):
//   * the detector fired (3 distinct targets blocked within 300 u);
//   * a live movement order the whole time: every end of the run came from a live loot leg and no gap between
//     two ends exceeded kOrderGap (the field's ends came ~9 s apart, one BLOCKED verdict is 3 s of Movement
//     Blocked plus a re-dispatch; 30 s is three of them);
//   * he moved less than kMovedMax (150 u, half the 300 u block radius, a few body lengths) from where the
//     run began, over at least kStuckSecs (20 s: two end-to-end gaps of the field cadence, ~9 s each, plus
//     margin; Followers::Refresh runs every ~0.5 s so 20 s is ~40 samples, and 1 s logistics ticks read it);
//   * not in combat (raw IsInCombat: this covers the Harbinger anchor-only leak), not talking, no world
//     transition inside the window (Followers::SecondsSinceTransition), the player farther than kPlayerNear
//     (300 u) -- if he can see him he may be doing it on purpose.
// THE DOOR ACTIVATION IS NOT BUILT. No engine proof exists in this repo that an NPC `ActivateRef` on a load
// door moves the actor through it (no MFO call site does it, ENGINE_NOTES has no entry, and no substitute
// such as MoveTo may be invented). So the rescue logs "WOULD RESCUE via door X at D u" and does nothing else.
// Once per episode.
//
// THREAD. Worker (the logistics tick) for NoteBlockedEnd / LootCooldownActive; ClearStuckOrigin from the
// revert path. One mutex covers the table. Session-only: nothing here is serialized.
#include "Logistics_internal.h"

namespace MFO::Logistics {

    namespace {
        // A run is "consecutive" while the ends keep coming: one BLOCKED end lasts ~3 s of Movement Blocked
        // plus a re-dispatch, so the field's 13 ends in ~2 min were ~9 s apart. 90 s is ten of those gaps.
        constexpr auto  kRunGap       = std::chrono::seconds(90);
        constexpr float kStuckRadius  = 300.0f;   // "about 300 u of one point" (marth's spec)
        constexpr int   kStuckTargets = 3;        // distinct targets
        constexpr auto  kOrderGap     = std::chrono::seconds(30);
        constexpr float kStuckSecs    = 20.0f;
        constexpr float kMovedMax     = 150.0f;
        constexpr float kPlayerNear   = 300.0f;
        constexpr float kDoorReach    = 4096.0f;
        struct Hit { RE::FormID target; RE::NiPoint3 pos; RE::NiPoint3 fpos; Clock::time_point at; };
        // suppress loot until he is > kStuckRadius from pos; the rest is the rescue's evidence
        struct Cool {
            RE::FormID cell; RE::NiPoint3 pos;
            Clock::time_point since{}; RE::NiPoint3 startPos{}; float maxMoved = 0.0f;
            bool orderLive = true; bool decided = false; Clock::time_point nextNearMiss{};
        };
        std::mutex                                      g_mx;
        std::unordered_map<RE::FormID, std::vector<Hit>> g_runs;   // follower -> current run
        std::unordered_map<RE::FormID, Cool>             g_cool;   // follower -> cooldown

        RE::FormID CellOf(const RE::Actor* a_f) {
            auto* c = a_f ? a_f->GetParentCell() : nullptr;
            return c ? c->GetFormID() : 0;
        }
    }

    bool StuckAtOriginNoteBlock(RE::Actor* a_follower, RE::FormID a_target, const RE::NiPoint3& a_blockPos,
                                Clock::time_point a_now, bool a_orderLive) {
        if (!a_follower || !a_target) return false;
        const RE::FormID id = a_follower->GetFormID();
        std::lock_guard lk(g_mx);
        auto& run = g_runs[id];
        // Keep only the trailing ends that are recent AND near the new point; anything else breaks the run.
        std::erase_if(run, [&](const Hit& h) {
            return a_now - h.at > kRunGap || h.pos.GetDistance(a_blockPos) > kStuckRadius;
        });
        std::erase_if(run, [&](const Hit& h) { return h.target == a_target; });   // distinct targets only
        run.push_back(Hit{ a_target, a_blockPos, a_follower->GetPosition(), a_now });
        if (static_cast<int>(run.size()) < kStuckTargets) return false;

        const int n = static_cast<int>(run.size());
        const auto here = a_follower->GetPosition();
        auto* cell = a_follower->GetParentCell();
        spdlog::error("[loot] {:08X} is stuck himself at ({:.0f},{:.0f},{:.0f}) -- {} different targets blocked at "
                      "the same spot; ending the excursion. PHYSICALLY STUCK: he stands at ({:.0f},{:.0f},{:.0f}), "
                      "swimming={}, cell {:08X} '{}'. No teleport (no ruling). No further loot excursions by him "
                      "until he is more than {:.0f} u from that point", id, a_blockPos.x, a_blockPos.y,
                      a_blockPos.z, n, here.x, here.y, here.z, a_follower->IsSwimming() ? "yes" : "no",
                      cell ? cell->GetFormID() : 0u, cell && cell->GetFullName() ? cell->GetFullName() : "?", kStuckRadius);
        Cool c{ CellOf(a_follower), here };
        c.since = run.front().at; c.startPos = run.front().fpos; c.orderLive = a_orderLive;
        for (std::size_t i = 1; i < run.size(); ++i)
            if (run[i].at - run[i - 1].at > kOrderGap) c.orderLive = false;
        for (const auto& h : run) c.maxMoved = std::max(c.maxMoved, h.fpos.GetDistance(c.startPos));
        g_cool[id] = c;
        run.clear();
        return true;
    }

    namespace {
        // The nearest not-locked DOOR within kDoorReach in his attached parent cell; a load door (ExtraTeleport)
        // wins over a plain one. Returns the ref or null; a_dist is its distance.
        RE::NiPointer<RE::TESObjectREFR> FindRescueDoor(RE::Actor* a_f, bool& a_load, float& a_dist) {
            RE::ObjectRefHandle bestLoad, bestPlain;
            float dLoad = kDoorReach + 1.0f, dPlain = kDoorReach + 1.0f;
            auto* cell = a_f->GetParentCell();
            if (!cell || !cell->IsAttached()) return {};
            const auto S = a_f->GetPosition();
            cell->ForEachReferenceInRange(S, kDoorReach, [&](RE::TESObjectREFR* r) {
                auto* base = r ? r->GetBaseObject() : nullptr;
                if (!base || !base->Is(RE::FormType::Door) || r->IsDisabled() || r->IsMarkedForDeletion() ||
                    r->IsLocked())
                    return RE::BSContainer::ForEachResult::kContinue;
                const float d = r->GetPosition().GetDistance(S);
                if (r->extraList.GetByType<RE::ExtraTeleport>()) {
                    if (d < dLoad) { dLoad = d; bestLoad = r->GetHandle(); }
                } else if (d < dPlain) { dPlain = d; bestPlain = r->GetHandle(); }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            if (auto p = bestLoad.get(); p) { a_load = true; a_dist = dLoad; return p; }
            if (auto p = bestPlain.get(); p) { a_load = false; a_dist = dPlain; return p; }
            return {};
        }
    }

    bool StuckAtOriginCooldown(RE::Actor* a_follower, Clock::time_point a_now) {
        if (!a_follower) return false;
        std::lock_guard lk(g_mx);
        if (g_cool.empty()) return false;
        auto it = g_cool.find(a_follower->GetFormID());
        if (it == g_cool.end()) return false;
        auto& c = it->second;
        const auto fpos = a_follower->GetPosition();
        if (c.cell != CellOf(a_follower) || c.pos.GetDistance(fpos) > kStuckRadius) {
            g_cool.erase(it);
            return false;
        }
        c.maxMoved = std::max(c.maxMoved, fpos.GetDistance(c.startPos));
        if (c.decided) return true;
        // ── the strict "really stuck" check (see the header). Evaluated each tick while suppressed. ──
        const float span  = std::chrono::duration<float>(a_now - c.since).count();
        auto*       pc    = RE::PlayerCharacter::GetSingleton();
        const float pDist = pc ? pc->GetPosition().GetDistance(fpos) : 1e9f;
        const float trans = Followers::SecondsSinceTransition();
        const bool  fight = a_follower->IsInCombat();
        const bool  talk  = a_follower->actorState2.talkingToPlayer;
        const bool okSpan = span >= kStuckSecs, okMove = c.maxMoved < kMovedMax, okTrans = trans < 0.0f || trans > span;
        const bool okPl   = pDist > kPlayerNear;
        const bool all    = c.orderLive && okSpan && okMove && !fight && !talk && okTrans && okPl;
        const std::string vals = std::format("movement order live={} | stuck for {:.1f}s (need >= {:.0f}) | moved {:.0f} u "
                                             "(need < {:.0f}) | in combat={} | talking={} | since a world transition {} "
                                             "(window {:.1f}s) | player {:.0f} u away (need > {:.0f})",
                                             c.orderLive ? "yes" : "no", span, kStuckSecs, c.maxMoved, kMovedMax,
                                             fight ? "yes" : "no", talk ? "yes" : "no",
                                             trans < 0.0f ? std::string("never") : std::format("{:.1f}s", trans),
                                             span, pDist, kPlayerNear);
        if (!all) {
            // A criterion that can never recover this episode (moved too far, order gap, fight) is a final
            // near-miss; one that can (span, player, transition) is re-checked. Either way, <= 1 line / 10 s.
            if (a_now >= c.nextNearMiss) {
                c.nextNearMiss = a_now + std::chrono::seconds(10);
                spdlog::info("[loot] {:08X} stuck-at-origin NEAR-MISS (the detector fired, the strict check failed): {}",
                             a_follower->GetFormID(), vals);
            }
            return true;
        }
        c.decided = true;   // once per episode
        bool load = false; float dist = 0.0f;
        auto  doorPtr = FindRescueDoor(a_follower, load, dist);
        auto* door    = doorPtr.get();
        spdlog::warn("[loot] {:08X} REALLY STUCK -- {}", a_follower->GetFormID(), vals);
        if (door)
            spdlog::warn("[loot] {:08X} WOULD RESCUE via {} door {:08X} ({}) at {:.0f} u -- stuck at ({:.0f},{:.0f},{:.0f}) "
                         "for {:.0f} s. NOT EXECUTED: an NPC activating a door is not verified to carry him through it, "
                         "and no substitute (MoveTo) is invented", a_follower->GetFormID(), load ? "LOAD" : "plain",
                         door->GetFormID(), door->GetName() ? door->GetName() : "?", dist, fpos.x, fpos.y, fpos.z, span);
        else
            spdlog::warn("[loot] {:08X} REALLY STUCK but no unlocked door within {:.0f} u of ({:.0f},{:.0f},{:.0f}) "
                         "in his cell -- no rescue to offer", a_follower->GetFormID(), kDoorReach, fpos.x, fpos.y, fpos.z);
        return true;
    }

    void ClearStuckAtOrigin() {
        std::lock_guard lk(g_mx);
        g_runs.clear();
        g_cool.clear();
    }

}
