// logistics/StuckAtOrigin.cpp -- a follower who is stuck at his OWN origin (field 2026-10-19, LoreRim Riften).
//
// WHAT HAPPENED. Sven was trapped for about 2 minutes at canal level (underwater, under stairs: marth). More than
// 13 DIFFERENT loot targets all ended BLOCKED at the same spot (a box at ~(172400..172680, -94135..-94300,
// z 10436..10527)). Each end was judged on its own: the target was GATED, the next one was tried, and it ended
// BLOCKED at the same place. The fault was never the targets, it was where HE stood.
//
// THE DETECTOR. Three or more consecutive static-block (GATED) ends, across DISTINCT targets, whose block points
// all lie within kStuckRadius of the newest one, with no other kind of end between them (a pickup, a teleport, an
// emptied target, an actor block, a lockpick door leg: Service.cpp calls StuckAtOriginResetRun): he is stuck
// himself OR behind a gate he cannot pass. The caller ends the excursion, releases the ch.19 road
// (Packages::LootTravelClear) and sends him back to follow. This opens an EPISODE: loot excursions by him are
// SUPPRESSED until he is more than kStuckRadius from where he stood (or leaves the cell). No timer.
//
// THE EVIDENCE WINDOW STARTS AT DETECTION (review of fb883fe3: the loot run's own samples cannot prove "stuck",
// because a blocked leg stands still by definition and a follower behind a closed gate walks off fine on follow).
// From the moment the episode opens, FOLLOW is his only order, and the episode is "really stuck" only when ALL hold
// (logged with each value; a failure is a rate-limited NEAR-MISS so the thresholds can be tuned):
//   * kStuckSecs (20 s) have passed since detection. The field's block ends came ~9 s apart (3 s of Movement
//     Blocked plus a re-dispatch); 20 s is two of those, and a follow package re-paths in a few seconds, so a free
//     follower has long since walked off;
//   * he moved less than kMovedMax (150 u, half the block radius, a few body lengths) from the detection point
//     over that time (sampled each logistics service, ~1 s);
//   * FOLLOW is a live order: the player is beyond follow distance (kPlayerNear, 300 u, also the "player can see
//     him and may be doing it on purpose" bar) and is not closing it (his distance to the player did not fall by
//     more than kClosingU, 100 u, since detection);
//   * he is not in combat (raw IsInCombat: this covers the Harbinger anchor-only leak), not talking, not mounted,
//     not on an errand, and no world transition lies inside the window (Followers::SecondsSinceTransition).
//
// THE RESCUE (marth 2026-10-19: "It should have him exit a door near his stuck position. but ONLY if hes really
// stuck."). Once per episode, after the check passes: on the MAIN thread, the nearest UNLOCKED LOAD door (an
// ExtraTeleport door, never a plain one: a plain door only opens) within kDoorReach (1500 u) of him is activated BY
// HIM: TESObjectREFR::ActivateRef(follower). PROVEN by disassembly on 1.6.1170 and 1.5.97 (review log, 2026-10-19):
// TESObjectDOOR::Activate (1.6 0x14027da50, 1.5 0x14022cb50), for an NPC with a high process, sets fadeState
// kTeleportOut via SetFade and returns FALSE; about 1 s later the high-process fade update re-activates the door
// (1.6 0x1406eb82b) and the teleport path moves him to the linked door. There is no distance check. It is refused
// for a mounted or interacting actor, a locked door without a key, or an actor with no process. The immediate
// false return is NOT a failure: kVerifyAfter (3 s) later the worker reads whether his cell changed or he moved
// more than kMovedAfter (500 u) and logs RESCUED, or a loud "rescue did NOT move him". ActivateRef is an
// id-based call: it carries a verified-address row (tools/verified_addresses/spec.json,
// "StuckAtOrigin.TESObjectREFR.ActivateRef") and a SeatVerified guard.
// The door search is on the main thread because it walks every loaded cell (TES::ForEachReferenceInRange, whose
// exterior grid walk is unsafe off the main thread: crash4).
//
// THREAD. Worker (the logistics service tick) for NoteBlock / ResetRun / Cooldown / Tick; the door search and the
// activation are posted to the MAIN thread (MainThread::Post) and re-resolve the follower by FormID.
// ClearStuckAtOrigin is the revert path. One mutex covers the table. Session-only: nothing here is serialized.
#include "Logistics_internal.h"
#include "Errand.h"        // Errand::Active -- an errand owns his movement
#include "Runtime.h"       // SeatVerified: the verified-address gate for the id-based ActivateRef
#include "MainThread.h"
#include "Followers.h"

namespace MFO::Logistics {

    namespace {
        // A run is "consecutive" while the ends keep coming: one BLOCKED end lasts ~3 s of Movement Blocked
        // plus a re-dispatch, so the field's 13 ends in ~2 min were ~9 s apart. 90 s is ten of those gaps.
        constexpr auto  kRunGap       = std::chrono::seconds(90);
        constexpr float kStuckRadius  = 300.0f;   // "about 300 u of one point" (marth's spec)
        constexpr int   kStuckTargets = 3;        // distinct targets
        constexpr float kStuckSecs    = 20.0f;
        constexpr float kMovedMax     = 150.0f;
        constexpr float kPlayerNear   = 300.0f;
        constexpr float kClosingU     = 100.0f;
        constexpr float kDoorReach    = 1500.0f;
        constexpr auto  kVerifyAfter  = std::chrono::seconds(3);
        constexpr auto  kPostLost     = std::chrono::seconds(15);   // a posted lambda that never ran: give the episode back
        constexpr float kMovedAfter   = 500.0f;

        struct Hit { RE::FormID target; RE::NiPoint3 pos; Clock::time_point at; };
        enum class Stage { Watching, Posted, Done };
        struct Episode {
            RE::FormID        cell = 0;
            RE::NiPoint3      pos{};            // where he stood at detection: the suppression anchor
            RE::NiPoint3      startPos{};       // same, the movement baseline
            Clock::time_point since{};          // DETECTION time: the evidence window starts here
            float             pDist0 = 0.0f;    // his distance to the player at detection
            float             maxMoved = 0.0f;
            Stage             stage = Stage::Watching;
            Clock::time_point nextNearMiss{};
            // the rescue
            Clock::time_point postedAt{};
            RE::NiPoint3      fromPos{};
            RE::FormID        fromCell = 0;
            float             spanAtPost = 0.0f;
            bool              doorFound = false;   // set by the main-thread lambda
            bool              activated = false;
            RE::FormID        doorId = 0;
            std::string       doorName;
            float             doorDist = 0.0f;
        };
        std::mutex                                       g_mx;
        std::unordered_map<RE::FormID, std::vector<Hit>> g_runs;   // follower -> current run
        std::unordered_map<RE::FormID, Episode>          g_eps;    // follower -> open episode

        RE::FormID CellOf(const RE::Actor* a_f) {
            auto* c = a_f ? a_f->GetParentCell() : nullptr;
            return c ? c->GetFormID() : 0;
        }

        // MAIN THREAD. Find the nearest unlocked LOAD door within kDoorReach and have him activate it. Re-resolves
        // the follower by FormID; records what it did in the episode for the worker's verification.
        void RescueOnMain(RE::FormID a_id) {
            auto* f = RE::TESForm::LookupByID<RE::Actor>(a_id);
            auto fail = [&](const char* a_why) {
                spdlog::warn("[loot] {:08X} stuck-at-origin rescue NOT attempted: {}", a_id, a_why);
                std::lock_guard lk(g_mx);
                if (auto it = g_eps.find(a_id); it != g_eps.end()) it->second.stage = Stage::Done;
            };
            if (!f || f->IsDead() || f->IsDisabled() || !f->Is3DLoaded()) return fail("he is gone, dead, disabled or unloaded");
            if (f->IsOnMount()) return fail("he is mounted (the engine refuses a mounted actor)");
            if (f->IsInCombat()) return fail("he is in combat");
            auto* tes = RE::TES::GetSingleton();
            if (!tes) return fail("no TES singleton");

            RE::ObjectRefHandle best;
            float bestD = kDoorReach + 1.0f;
            const auto S = f->GetPosition();
            tes->ForEachReferenceInRange(f, kDoorReach, [&](RE::TESObjectREFR* r) {
                auto* base = r ? r->GetBaseObject() : nullptr;
                if (!base || !base->Is(RE::FormType::Door) || r->IsDisabled() || r->IsMarkedForDeletion() ||
                    r->IsLocked())
                    return RE::BSContainer::ForEachResult::kContinue;
                auto* et = r->extraList.GetByType<RE::ExtraTeleport>();
                if (!et || !et->teleportData)   // a LOAD door only: a plain door just opens
                    return RE::BSContainer::ForEachResult::kContinue;
                const float d = r->GetPosition().GetDistance(S);
                if (d < bestD) { bestD = d; best = r->GetHandle(); }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            auto  dptr = best.get();
            auto* door = dptr.get();
            if (!door) {
                spdlog::warn("[loot] {:08X} REALLY STUCK but no door near him (no unlocked load door within {:.0f} u of "
                             "({:.0f},{:.0f},{:.0f})) -- nothing done", a_id, kDoorReach, S.x, S.y, S.z);
                std::lock_guard lk(g_mx);
                if (auto it = g_eps.find(a_id); it != g_eps.end()) it->second.stage = Stage::Done;
                return;
            }
            // The verified-address gate for the id-based call (CLAUDE.md SEAT SELF-CHECK).
            static const bool verified = [] {
                REL::Relocation<std::uintptr_t> fn{ REL::RelocationID(19369, 19796) };
                return Runtime::SeatVerified(fn.address(), "StuckAtOrigin.TESObjectREFR.ActivateRef");
            }();
            if (!verified) return fail("the ActivateRef address is not verified for this build (seat refused)");
            const char* nm = door->GetName();
            {
                std::lock_guard lk(g_mx);
                if (auto it = g_eps.find(a_id); it != g_eps.end()) {
                    it->second.doorFound = true;
                    it->second.doorId    = door->GetFormID();
                    it->second.doorName  = nm && *nm ? nm : "?";
                    it->second.doorDist  = bestD;
                }
            }
            spdlog::warn("[loot] {:08X} stuck-at-origin: activating LOAD door {:08X} ({}) at {:.0f} u as him; the first "
                         "Activate returns false by design (the engine fade teleports him ~1 s later)",
                         a_id, door->GetFormID(), nm && *nm ? nm : "?", bestD);
            door->ActivateRef(f, 0, nullptr, 1, false);   // the false return is the fade starting, not a failure
            std::lock_guard lk(g_mx);
            if (auto it = g_eps.find(a_id); it != g_eps.end()) it->second.activated = true;
        }

        // Worker: read the outcome of a posted rescue once kVerifyAfter has passed (any follower's tick calls this, so
        // a follower the engine unloaded is still reported). g_mx NOT held on entry.
        void VerifyPosted(Clock::time_point a_now) {
            struct Out { RE::FormID id; Episode e; };
            std::vector<Out> due;
            {
                std::lock_guard lk(g_mx);
                for (auto& [id, e] : g_eps) {
                    if (e.stage != Stage::Posted) continue;
                    if (e.activated && a_now - e.postedAt >= kVerifyAfter) {
                        due.push_back({ id, e });
                        e.stage = Stage::Done;   // once per episode, whatever the result
                    } else if (!e.doorFound && a_now - e.postedAt >= kPostLost) {
                        e.stage = Stage::Done;   // the main-thread lambda never ran: do not hold the episode forever
                        spdlog::warn("[loot] {:08X} stuck-at-origin rescue was posted {}s ago and never ran", id,
                                     std::chrono::duration_cast<std::chrono::seconds>(a_now - e.postedAt).count());
                    }
                }
            }
            for (auto& o : due) {
                auto* f = RE::TESForm::LookupByID<RE::Actor>(o.id);
                auto* cell = f ? f->GetParentCell() : nullptr;
                // Unresolved / unloaded / no cell = he left the loaded area: moved.
                const bool  gone = !f || !f->Is3DLoaded() || !cell;
                const float mv   = gone ? 0.0f : f->GetPosition().GetDistance(o.e.fromPos);
                const bool  newCell = !gone && CellOf(f) != o.e.fromCell;
                if (gone || newCell || mv > kMovedAfter)
                    spdlog::warn("[loot] {:08X} RESCUED via door {:08X} ({}) at {:.0f} u -- stuck at ({:.0f},{:.0f},{:.0f}) "
                                 "for {:.0f} s{}", o.id, o.e.doorId, o.e.doorName, o.e.doorDist, o.e.fromPos.x,
                                 o.e.fromPos.y, o.e.fromPos.z, o.e.spanAtPost,
                                 gone ? " (he left the loaded area)" : (newCell ? " (new cell)" : ""));
                else
                    spdlog::error("[loot] {:08X} rescue did NOT move him: door {:08X} ({}) at {:.0f} u was activated {}s ago "
                                  "and he is still within {:.0f} u of ({:.0f},{:.0f},{:.0f}) in the same cell. No second "
                                  "rescue this episode", o.id, o.e.doorId, o.e.doorName, o.e.doorDist,
                                  std::chrono::duration_cast<std::chrono::seconds>(kVerifyAfter).count(), mv,
                                  o.e.fromPos.x, o.e.fromPos.y, o.e.fromPos.z);
            }
        }
    }

    void StuckAtOriginResetRun(RE::FormID a_follower) {
        std::lock_guard lk(g_mx);
        if (auto it = g_runs.find(a_follower); it != g_runs.end()) it->second.clear();
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
        const auto here = a_follower->GetPosition();
        auto* cell = a_follower->GetParentCell();
        auto* pc = RE::PlayerCharacter::GetSingleton();
        const auto* st = a_follower->AsActorState();
        spdlog::error("[loot] {:08X} is stuck himself at ({:.0f},{:.0f},{:.0f}) -- {} different targets blocked at "
                      "the same spot; ending the excursion. He stands at ({:.0f},{:.0f},{:.0f}), swimming={}, cell "
                      "{:08X} '{}'. No further loot excursions by him until he is more than {:.0f} u from that point; "
                      "the 'really stuck' check starts now ({:.0f} s of follow, then the door rescue if every criterion holds)",
                      id, a_blockPos.x, a_blockPos.y, a_blockPos.z, n, here.x, here.y, here.z,
                      (st && st->IsSwimming()) ? "yes" : "no",
                      cell ? cell->GetFormID() : 0u, cell && cell->GetFullName() ? cell->GetFullName() : "?",
                      kStuckRadius, kStuckSecs);
        Episode e;
        e.cell     = CellOf(a_follower);
        e.pos      = here;
        e.startPos = here;
        e.since    = a_now;
        e.pDist0   = pc ? pc->GetPosition().GetDistance(here) : 1e9f;
        g_eps[id]  = e;
        run.clear();
        return true;
    }

    // Pure query for LootNearby: is loot suppressed for him (an open episode)? The episode is closed by
    // StuckAtOriginTick (it reports a rescue before it forgets the episode).
    bool StuckAtOriginCooldown(RE::Actor* a_follower, Clock::time_point) {
        if (!a_follower) return false;
        std::lock_guard lk(g_mx);
        return !g_eps.empty() && g_eps.contains(a_follower->GetFormID());
    }

    // The logistics service tick (Logistics::ServiceFollower, every service, BEFORE rule evaluation: independent of
    // whether any loot rule dispatches). Worker.
    void StuckAtOriginTick(RE::Actor* a_follower, Clock::time_point a_now) {
        VerifyPosted(a_now);   // any follower's tick reports any posted rescue (he may be unloaded by now)
        if (!a_follower) return;
        const RE::FormID id = a_follower->GetFormID();
        {
            std::lock_guard lk(g_mx);
            if (g_eps.empty()) return;
            auto it = g_eps.find(id);
            if (it == g_eps.end()) return;
            auto& c = it->second;
            if (c.stage == Stage::Posted) return;   // VerifyPosted owns it until the result is logged
            const auto fpos = a_follower->GetPosition();
            // The episode ends (loot allowed again) when he is far from the stuck point or in another cell.
            const bool cellChanged = c.cell != CellOf(a_follower);
            if (cellChanged || c.pos.GetDistance(fpos) > kStuckRadius) {
                spdlog::info("[loot] {:08X} left the stuck point ({:.0f} u, cell {}) -- loot excursions allowed again",
                             id, c.pos.GetDistance(fpos), cellChanged ? "changed" : "same");
                g_eps.erase(it);
                return;
            }
            c.maxMoved = std::max(c.maxMoved, fpos.GetDistance(c.startPos));
            if (c.stage == Stage::Done) return;
            // ── the strict "really stuck" check (see the header). ──
            auto*       pc    = RE::PlayerCharacter::GetSingleton();
            const float span  = std::chrono::duration<float>(a_now - c.since).count();
            const float pDist = pc ? pc->GetPosition().GetDistance(fpos) : 1e9f;
            const float trans = Followers::SecondsSinceTransition();
            const bool  fight = a_follower->IsInCombat();
            const auto* st    = a_follower->AsActorState();
            const bool  talk  = st && st->actorState2.talkingToPlayer;
            const bool  mount = a_follower->IsOnMount();
            const bool  errnd = Errand::Active(id);
            const bool  closing = pDist < c.pDist0 - kClosingU;
            const bool  okSpan = span >= kStuckSecs, okMove = c.maxMoved < kMovedMax;
            const bool  okTrans = trans < 0.0f || trans > span;
            const bool  okFollow = pDist > kPlayerNear && !closing;
            const bool  all = okSpan && okMove && okFollow && !fight && !talk && !mount && !errnd && okTrans;
            const std::string transTxt = trans < 0.0f ? std::string("never") : std::format("{:.1f}s", trans);
            if (!all) {
                if (a_now >= c.nextNearMiss) {
                    c.nextNearMiss = a_now + std::chrono::seconds(10);
                    spdlog::info("[loot] {:08X} stuck-at-origin NEAR-MISS (detector fired, strict check not satisfied): "
                                 "{:.1f}s since detection (need >= {:.0f}) | moved {:.0f} u (need < {:.0f}) | follow live "
                                 "(player {:.0f} u away, need > {:.0f}, closing={}) {} | in combat={} | talking={} | "
                                 "mounted={} | on errand={} | since a world transition {} (window {:.1f}s)",
                                 id, span, kStuckSecs, c.maxMoved, kMovedMax, pDist, kPlayerNear, closing ? "yes" : "no",
                                 okFollow ? "yes" : "NO", fight ? "yes" : "no", talk ? "yes" : "no", mount ? "yes" : "no",
                                 errnd ? "yes" : "no", transTxt, span);
                }
                return;
            }
            spdlog::warn("[loot] {:08X} REALLY STUCK -- {:.1f}s since detection | moved {:.0f} u | follow live (player {:.0f} "
                         "u away, closing={}) | in combat=no | talking=no | mounted=no | on errand=no | since a world "
                         "transition {}", id, span, c.maxMoved, pDist, closing ? "yes" : "no", transTxt);
            c.stage      = Stage::Posted;
            c.postedAt   = a_now;
            c.fromPos    = fpos;
            c.fromCell   = CellOf(a_follower);
            c.spanAtPost = span;
        }
        if (!MainThread::IsInstalled()) {   // VR: no pump, no rescue; said once, not masked
            spdlog::warn("[loot] {:08X} REALLY STUCK but there is no main-thread pump -- no rescue", id);
            std::lock_guard lk(g_mx);
            if (auto it = g_eps.find(id); it != g_eps.end()) it->second.stage = Stage::Done;
            return;
        }
        MainThread::Post([id]() { RescueOnMain(id); });
    }

    void ClearStuckAtOrigin() {
        std::lock_guard lk(g_mx);
        g_runs.clear();
        g_eps.clear();
    }

}
