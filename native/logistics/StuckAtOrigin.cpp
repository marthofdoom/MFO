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
// COOLDOWN that stops the very next tick from dispatching another loot leg from the same box.
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
        // The cooldown: 3 x fExcursionMax's 60 s default. The excursion cap is the longest single outing; a
        // follower who failed 3 of them from one spot should sit out three outing-lengths while the player
        // walks on (a player at walking pace leaves a 2048-u leash-cell in well under that). It is also
        // scoped to the CELL, so crossing into another cell lifts it at once, and it is session-only.
        constexpr auto  kCooldown     = std::chrono::seconds(180);

        struct Hit { RE::FormID target; RE::NiPoint3 pos; Clock::time_point at; };
        struct Cool { RE::FormID cell; Clock::time_point until; };
        std::mutex                                      g_mx;
        std::unordered_map<RE::FormID, std::vector<Hit>> g_runs;   // follower -> current run
        std::unordered_map<RE::FormID, Cool>             g_cool;   // follower -> cooldown

        RE::FormID CellOf(const RE::Actor* a_f) {
            auto* c = a_f ? a_f->GetParentCell() : nullptr;
            return c ? c->GetFormID() : 0;
        }
    }

    bool StuckAtOriginNoteBlock(RE::Actor* a_follower, RE::FormID a_target, const RE::NiPoint3& a_blockPos,
                                Clock::time_point a_now) {
        if (!a_follower || !a_target) return false;
        const RE::FormID id = a_follower->GetFormID();
        std::lock_guard lk(g_mx);
        auto& run = g_runs[id];
        // Keep only the trailing ends that are recent AND near the new point; anything else breaks the run.
        std::erase_if(run, [&](const Hit& h) {
            return a_now - h.at > kRunGap || h.pos.GetDistance(a_blockPos) > kStuckRadius;
        });
        std::erase_if(run, [&](const Hit& h) { return h.target == a_target; });   // distinct targets only
        run.push_back(Hit{ a_target, a_blockPos, a_now });
        if (static_cast<int>(run.size()) < kStuckTargets) return false;

        const int n = static_cast<int>(run.size());
        spdlog::error("[loot] {:08X} is stuck himself at ({:.0f},{:.0f},{:.0f}) -- {} different targets blocked at "
                      "the same spot; ending the excursion (no further loot excursions by him in this cell for "
                      "{}s)", id, a_blockPos.x, a_blockPos.y, a_blockPos.z, n,
                      std::chrono::duration_cast<std::chrono::seconds>(kCooldown).count());
        g_cool[id] = Cool{ CellOf(a_follower), a_now + kCooldown };
        run.clear();
        return true;
    }

    bool StuckAtOriginCooldown(RE::Actor* a_follower, Clock::time_point a_now) {
        if (!a_follower) return false;
        std::lock_guard lk(g_mx);
        if (g_cool.empty()) return false;
        auto it = g_cool.find(a_follower->GetFormID());
        if (it == g_cool.end()) return false;
        if (a_now >= it->second.until || it->second.cell != CellOf(a_follower)) {
            g_cool.erase(it);
            return false;
        }
        return true;
    }

    void ClearStuckAtOrigin() {
        std::lock_guard lk(g_mx);
        g_runs.clear();
        g_cool.clear();
    }

}
