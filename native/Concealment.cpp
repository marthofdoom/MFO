#include "PCH.h"
#include "Concealment.h"

#include "Confidence.h"          // LeashRadius: the pick stays inside the follower's leash
#include "Config.h"
#include "MainThread.h"
#include "Sightline.h"           // PointSeenNow: MFO's own ray from a foe's eye to a point
#include "apmf/APMFBridge.h"     // the disengage drive (H3), FindStandPoint, the G1 stance claim

#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

// Card G3. The header has the contract. What each constant is sized from:
//   kRing / kBearings     the candidates: 12 bearings on two rings around him. FindEmptySpace walks a straight chest-height
//                         ray from him, so every candidate is a point he can walk to in a line (a corner, a pillar, a
//                         rock, a doorway). Points around a second corner are NOT found (a stated limit).
//   kPerFrame             candidates tested per frame: at most 4 x (one FindEmptySpace of <= 14 rays + 6 foes x 3 rays).
//   kRepickMs / kFreshMs  one pick per 1.5 s per follower at most; an answer is used for 3 s (he and the foes move).
//   kRunMaxMs             the run's ceiling. 600 u at the engine's run speed is under 2 s; 10 s covers a path around
//                         furniture. Past it the run is a FAILURE, said out loud (principle 7), never retried silently.
//   kHideHoldMs           the hold at the point: the engine loses an unwatched actor after fCombatDetectionLostTimeLimit
//                         (10 s, calculate-detection-re.md), a watched one at once. 12 s is that floor plus a margin
//                         (principle 9). The hold ends early when he is hit there (he was found).
namespace MFO::Concealment {

    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr int   kBearings = 12;
        constexpr float kRing[2]  = { 320.0f, 600.0f };
        constexpr int   kCandidates = kBearings * 2;
        constexpr int   kPerFrame = 4;
        constexpr std::size_t kMaxFoes = 6;
        constexpr float kClearance  = 40.0f;    // the free radius wanted around the point
        constexpr float kMinMove    = 160.0f;   // a walk clamped by a wall beside him is not a move
        constexpr float kMinFoeDist = 250.0f;   // never a point beside a foe, however the rays read
        constexpr float kLeashMargin = 200.0f;  // the point's distance inside the ch.23 leash (anchor: the player)
        constexpr float kRadius     = 90.0f;    // R of the drive: inside it he has arrived
        constexpr std::chrono::milliseconds kRepickMs{ 1500 };
        constexpr std::chrono::milliseconds kFreshMs{ 3000 };
        constexpr std::chrono::milliseconds kRunMaxMs{ 10000 };
        constexpr std::chrono::milliseconds kHideHoldMs{ 12000 };

        struct Pick {
            RE::NiPoint3      point{};
            Clock::time_point at{};        // when the answer landed
            Clock::time_point askedAt{};
            bool              valid = false;
            bool              pending = false;
        };
        enum class Phase : std::uint8_t { None, Running, Hiding };
        struct Run {
            Phase             phase = Phase::None;
            RE::NiPoint3      point{};
            Clock::time_point startedAt{}, arrivedAt{};
            float             hpAtArrival = 0.0f;
            bool              sneakNoteLogged = false;
        };
        std::mutex                           g_mx;      // leaf: never held across an engine or Harbinger call
        std::unordered_map<RE::FormID, Pick> g_picks;
        std::unordered_map<RE::FormID, Run>  g_runs;
        std::uint64_t                        g_epoch = 0;   // bumped by ClearTransientState: a pick of the old world is dropped

        struct Job {
            RE::FormID              follower = 0;
            std::vector<RE::FormID> foes;
            float                   leash = 0.0f;   // Confidence::LeashRadius, read on the worker in Prepare
            std::uint64_t           epoch = 0;
            int                     next = 0;
            int                     standable = 0, hidden = 0, seen = 0, unknown = 0, tooNear = 0, offLeash = 0, noMove = 0;
            bool                    have = false;
            RE::NiPoint3            best{};
            float                   bestCost = 0.0f;
        };

        void Finish(const std::shared_ptr<Job>& a_job, const char* a_abort) {
            {
                std::scoped_lock lk(g_mx);
                if (a_job->epoch != g_epoch) return;
                Pick& p   = g_picks[a_job->follower];
                p.pending = false;
                p.valid   = !a_abort && a_job->have;
                p.point   = a_job->best;
                p.at      = Clock::now();
            }
            if (a_abort)
                spdlog::info("[conceal] {:08X}: pick dropped ({})", a_job->follower, a_abort);
            else if (a_job->have)
                spdlog::info("[conceal] {:08X}: concealment point {:.0f},{:.0f},{:.0f} ({:.0f}u from him), hidden from all {} foe(s) by "
                             "MFO's own rays | {} candidates: standable {}, hidden {}, seen {}, ray unknown {}, beside a foe {}, "
                             "outside the leash {}, no real move {}",
                             a_job->follower, a_job->best.x, a_job->best.y, a_job->best.z, a_job->bestCost, a_job->foes.size(),
                             kCandidates, a_job->standable, a_job->hidden, a_job->seen, a_job->unknown, a_job->tooNear,
                             a_job->offLeash, a_job->noMove);
            else
                spdlog::info("[conceal] {:08X}: NO concealment point within {:.0f}u ({} foe(s)) | {} candidates: standable {}, hidden "
                             "{}, seen {}, ray unknown {}, beside a foe {}, outside the leash {}, no real move {}",
                             a_job->follower, kRing[1], a_job->foes.size(), kCandidates, a_job->standable, a_job->hidden,
                             a_job->seen, a_job->unknown, a_job->tooNear, a_job->offLeash, a_job->noMove);
        }

        // MAIN THREAD. kPerFrame candidates, then itself again on the next frame until all are tested.
        void PickStep(std::shared_ptr<Job> a_job) {
            {
                std::scoped_lock lk(g_mx);
                if (a_job->epoch != g_epoch) return;   // a load replaced the world
            }
            auto* f = RE::TESForm::LookupByID<RE::Actor>(a_job->follower);
            if (!f || f->IsDead() || !f->Is3DLoaded()) return Finish(a_job, "the follower is gone");
            // The foes, resolved fresh each frame (ids, never pointers kept across frames).
            std::vector<RE::Actor*> foes;
            for (const RE::FormID id : a_job->foes)
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && !a->IsDead() && a->Is3DLoaded()) foes.push_back(a);
            if (foes.empty()) return Finish(a_job, "no listed foe is loaded and alive: nothing to hide from");
            const RE::NiPoint3 me = f->GetPosition();
            float              height = f->GetHeight();
            if (!(height > 1.0f)) height = 120.0f;
            auto*       pc    = RE::PlayerCharacter::GetSingleton();
            const float leash = a_job->leash;

            for (int n = 0; n < kPerFrame && a_job->next < kCandidates; ++n, ++a_job->next) {
                const int   i       = a_job->next;
                const float heading = static_cast<float>(i % kBearings) * (6.2831853f / static_cast<float>(kBearings));
                const float dist    = kRing[i / kBearings];
                RE::NiPoint3  pt{};
                std::uint32_t status = 0;
                if (!APMFBridge::FindStandPoint(a_job->follower, heading, dist, kClearance, pt, status)) continue;
                ++a_job->standable;
                const float cost = me.GetDistance(pt);
                if (cost < kMinMove) { ++a_job->noMove; continue; }
                if (pc && leash > 0.0f && pc->GetPosition().GetDistance(pt) > leash - kLeashMargin) { ++a_job->offLeash; continue; }
                bool beside = false, seen = false, unknown = false;
                for (RE::Actor* foe : foes) {
                    if (foe->GetPosition().GetDistance(pt) < kMinFoeDist) { beside = true; break; }
                    const auto ps = Sightline::PointSeenNow(foe, pt, height);
                    if (ps == Sightline::PointSight::Seen) { seen = true; break; }
                    if (ps == Sightline::PointSight::Unavailable) { unknown = true; break; }   // never read as hidden
                }
                if (beside) { ++a_job->tooNear; continue; }
                if (seen) { ++a_job->seen; continue; }
                if (unknown) { ++a_job->unknown; continue; }
                ++a_job->hidden;
                if (!a_job->have || cost < a_job->bestCost) {   // the nearest hidden point: the shortest time in the open
                    a_job->have     = true;
                    a_job->best     = pt;
                    a_job->bestCost = cost;
                }
            }
            if (a_job->next >= kCandidates) return Finish(a_job, nullptr);
            MainThread::Post([a_job]() { PickStep(a_job); });
        }

        float Health(RE::Actor* a) { return a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth); }
    }

    bool RoadOpen() {
        return Config::g_iausHideRoad.load(std::memory_order_relaxed) && APMFBridge::DisengageSupported() &&
               MainThread::IsInstalled();
    }

    bool Prepare(RE::Actor* a_actor, const std::vector<RE::FormID>& a_foes) {
        if (!a_actor || a_foes.empty() || !RoadOpen()) return false;
        const RE::FormID     a_follower = a_actor->GetFormID();
        const float          leash = Confidence::LeashRadius(a_actor);   // worker read, as the scheduler's own
        std::shared_ptr<Job> job;
        bool                 fresh = false;
        {
            std::scoped_lock lk(g_mx);
            if (const auto r = g_runs.find(a_follower); r != g_runs.end() && r->second.phase != Phase::None) return false;
            Pick&      p   = g_picks[a_follower];
            const auto now = Clock::now();
            fresh = p.valid && now - p.at < kFreshMs;
            if (!p.pending && now - p.askedAt >= kRepickMs) {
                p.pending = true;
                p.askedAt = now;
                job = std::make_shared<Job>();
                job->follower = a_follower;
                job->epoch    = g_epoch;
                job->leash    = leash;
                job->foes.assign(a_foes.begin(), a_foes.begin() + static_cast<std::ptrdiff_t>((std::min)(a_foes.size(), kMaxFoes)));
            }
        }
        if (job) MainThread::Post([job]() { PickStep(job); });   // outside our lock (MainThread has its own queue mutex)
        return fresh;
    }

    const char* Begin(RE::Actor* a_follower, std::string& a_desc) {
        if (!a_follower) return "no follower";
        if (!RoadOpen()) return "no hide road ([Debug] bIausHideRoad off, Harbinger absent or older than ABI 39, or no main-thread pump)";
        const RE::FormID id = a_follower->GetFormID();
        RE::NiPoint3     pt{};
        {
            std::scoped_lock lk(g_mx);
            if (const auto r = g_runs.find(id); r != g_runs.end() && r->second.phase != Phase::None) return "a hide run already stands";
            const auto p = g_picks.find(id);
            if (p == g_picks.end() || !p->second.valid || Clock::now() - p->second.at >= kFreshMs)
                return "no fresh concealment point (the pick found none, or its answer is older than 3 s)";
            pt = p->second.point;
            p->second.valid = false;   // one run per answer
        }
        if (const char* why = APMFBridge::RequestDisengage(a_follower, pt, kRadius, /*a_denyOffense=*/true)) return why;
        {
            std::scoped_lock lk(g_mx);
            Run& r      = g_runs[id];
            r           = Run{};
            r.phase     = Phase::Running;
            r.point     = pt;
            r.startedAt = Clock::now();
        }
        a_desc = fmt::format("disengaging to a concealment point {:.0f}u away ({:.0f},{:.0f},{:.0f})",
                             a_follower->GetPosition().GetDistance(pt), pt.x, pt.y, pt.z);
        return nullptr;
    }

    bool Service(RE::Actor* a_follower) {
        if (!a_follower) return false;
        const RE::FormID id = a_follower->GetFormID();
        Run run;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_runs.find(id);
            if (it == g_runs.end() || it->second.phase == Phase::None) return false;
            run = it->second;
        }
        const auto now = Clock::now();
        auto end = [&](const char* a_why, bool a_failure) {
            {
                std::scoped_lock lk(g_mx);
                g_runs.erase(id);
            }
            APMFBridge::ReleaseDisengage(id, a_why);
            if (a_failure) spdlog::warn("[conceal] {:08X}: hide run FAILED: {}. Not retried until its cooldown is over.", id, a_why);
            else spdlog::info("[conceal] {:08X}: hide run over: {}", id, a_why);
            return false;
        };

        // The engine reports him out of his OWN combat (a Harbinger substrate is not a fight). Harbinger ends the ch.24
        // claim on that itself (CombatEnded), so the run has no road left. At the point this is what the run is for
        // (no foe holds him as a target any more); on the way it can also be the engine's flag dropping for a moment.
        // Either way the claims are released and the next lap decides again. Not a failure, and said as what it is.
        if (!APMFBridge::InCombat(a_follower))
            return end(run.phase == Phase::Hiding ? "the engine reports him out of combat at the point (no foe holds him)"
                                                  : "the engine reports him out of combat on the way", false);

        if (run.phase == Phase::Running) {
            std::string why;
            const auto  st = APMFBridge::ServiceDisengage(id, why);
            if (st == APMFBridge::DisengageState::None) return end("the drive is gone (released elsewhere)", true);
            if (st == APMFBridge::DisengageState::Failed) return end(why.c_str(), true);
            if (st == APMFBridge::DisengageState::Running) {
                if (now - run.startedAt > kRunMaxMs) return end("he did not reach the point in 10 s", true);
                return true;
            }
            // Arrived: the crouch and the hold start now.
            run.phase       = Phase::Hiding;
            run.arrivedAt   = now;
            run.hpAtArrival = Health(a_follower);
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_runs.find(id);
                if (it == g_runs.end()) return false;
                it->second.phase       = Phase::Hiding;
                it->second.arrivedAt   = run.arrivedAt;
                it->second.hpAtArrival = run.hpAtArrival;
            }
            spdlog::info("[conceal] {:08X}: at the concealment point after {:.1f} s: crouching (the G1 stance claim) and holding "
                         "for up to {} s while the engine loses him by its own rules",
                         id, std::chrono::duration<float>(now - run.startedAt).count(), kHideHoldMs.count() / 1000);
        }

        // Hiding. The G1 stance claim, re-read each lap (it files once and refreshes; apmf/SneakClaims.cpp).
        if (const char* why = APMFBridge::ServiceSneakRule(a_follower, APMFBridge::SneakTable::Combat); why && !run.sneakNoteLogged) {
            {
                std::scoped_lock lk(g_mx);
                if (const auto it = g_runs.find(id); it != g_runs.end()) it->second.sneakNoteLogged = true;
            }
            spdlog::warn("[conceal] {:08X}: at the point but NOT sneaking: {}. He holds there standing; the engine's own rules "
                         "decide whether a foe loses him.", id, why);
        }
        if (Health(a_follower) < run.hpAtArrival - 1.0f) return end("he was hit at the point (a foe found him)", false);
        if (now - run.arrivedAt > kHideHoldMs) return end("the hide hold's time is up", false);
        return true;
    }

    void Release(RE::FormID a_follower, const char* a_why) {
        bool had = false;
        {
            std::scoped_lock lk(g_mx);
            had = g_runs.erase(a_follower) != 0;
            g_picks.erase(a_follower);
        }
        APMFBridge::ReleaseDisengage(a_follower, a_why);   // idempotent (a drive Begin filed and the run lost track of)
        if (had) spdlog::info("[conceal] {:08X}: hide run released ({})", a_follower, a_why ? a_why : "");
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mx);
        ++g_epoch;
        g_picks.clear();
        g_runs.clear();
    }
}
