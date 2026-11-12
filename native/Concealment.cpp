#include "PCH.h"
#include "Concealment.h"

#include "Confidence.h"          // LeashRadius: the pick stays inside the follower's leash
#include "Config.h"
#include "MainThread.h"
#include "Sightline.h"           // PointSeenNow: MFO's own ray from a foe's eye to a point
#include "apmf/APMFBridge.h"     // the disengage drive (H3), FindStandPoint, the G1 stance claim
#include "iaus/Disengage.h"       // kFleeHealthLine
#include "iaus/Engine.h"         // NoteDisengageEnded: the cooldown counts from the end of the run
#include "iaus/Hide.h"           // G2 Layer 1's verdict: does this foe's group still have him

#include <chrono>
#include <cmath>
#include <limits>
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
//   kHideHoldMs           the UPPER BOUND of the hold at the point (marth 2026-11-11: the run ends the moment the engine says no
//                         foe detects him; this is only the cap). The engine loses an unwatched actor after
//                         fCombatDetectionLostTimeLimit (10 s, calculate-detection-re.md), a watched one at once; 12 s is
//                         that floor plus a margin (principle 9). Hitting the cap is logged. The hold also ends early when
//                         he is hit there (he was found).
//   kHiddenReads          the hide run ends only after this many CONSECUTIVE laps on which no foe detects him (re-review F2 of
//                         ba1328c): one lap can fall between two of the engine's own detection updates.
//   kFleeGain             a non-sneak FLEE point must stand this much farther from the nearest foe than he does now (chosen).
//   kFleeHealthLine       the flee ends once he is healed past the retreat's own line (iaus/Disengage.h).
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
        constexpr float kFleeGain   = 150.0f;   // a flee point: at least this much farther from the nearest foe than he is now
        constexpr std::chrono::milliseconds kRepickMs{ 1500 };
        constexpr std::chrono::milliseconds kFreshMs{ 3000 };
        constexpr std::chrono::milliseconds kRunMaxMs{ 10000 };
        constexpr std::chrono::milliseconds kHideHoldMs{ 12000 };
        constexpr int kHiddenReads = 2;

        struct Pick {
            RE::NiPoint3      point{};
            Clock::time_point at{};        // when the answer landed
            Clock::time_point askedAt{};
            bool              valid = false;
            bool              pending = false;
            bool              flee = false;              // the answer is for a flee (a point away from the foes), not a hide
            std::vector<RE::FormID> foes;                // the foes the answer was picked against (the run reads them again)
        };
        enum class Phase : std::uint8_t { None, Running, Hiding };
        struct Run {
            Phase             phase = Phase::None;
            RE::NiPoint3      point{};
            Clock::time_point startedAt{}, arrivedAt{};
            float             hpAtArrival = 0.0f;
            bool              sneakNoteLogged = false;
            bool              flee = false;              // a non-sneak flee: the IAUS keeps running (it heals), the scan is not skipped
            int               hiddenReads = 0;           // consecutive laps on which no foe detected him (the hide run ends at kHiddenReads)
            std::vector<RE::FormID> foes;                // the pick's foes; every lap reads these AND his current foes (WatchList)
        };
        struct Stance {                                  // the hide hold after a hidden run: the G1 claim stays until a blow / reveal / end
            std::vector<RE::FormID> foes;
        };
        std::mutex                           g_mx;      // leaf: never held across an engine or Harbinger call
        std::unordered_map<RE::FormID, Pick> g_picks;
        std::unordered_map<RE::FormID, Run>  g_runs;
        std::unordered_map<RE::FormID, Stance> g_stances;
        std::uint64_t                        g_epoch = 0;   // bumped by ClearTransientState: a pick of the old world is dropped

        struct Job {
            RE::FormID              follower = 0;
            std::vector<RE::FormID> foes;
            float                   leash = 0.0f;   // Confidence::LeashRadius, read on the worker in Prepare
            bool                    flee = false;
            float                   nearestNow = 0.0f;   // flee: his own distance to the nearest foe when the pick began
            std::uint64_t           epoch = 0;
            int                     next = 0;
            int                     standable = 0, hidden = 0, seen = 0, unknown = 0, tooNear = 0, offLeash = 0, noMove = 0;
            bool                    have = false;
            RE::NiPoint3            best{};
            float                   bestCost = 0.0f;
            float                   bestFoeDist = 0.0f;   // flee: the nearest-foe distance of the best point (the farthest wins)
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
                p.flee    = a_job->flee;
                p.foes    = a_job->foes;
            }
            if (a_abort)
                spdlog::info("[conceal] {:08X}: pick dropped ({})", a_job->follower, a_abort);
            else if (a_job->have && a_job->flee)
                spdlog::info("[conceal] {:08X}: FLEE point {:.0f},{:.0f},{:.0f} ({:.0f}u from him), {:.0f}u from the nearest of {} foe(s) "
                             "(he stands {:.0f}u from it now) | {} candidates: standable {}, beside a foe {}, not far enough from the "
                             "foes {}, outside the leash {}, no real move {}",
                             a_job->follower, a_job->best.x, a_job->best.y, a_job->best.z, a_job->bestCost, a_job->bestFoeDist,
                             a_job->foes.size(), a_job->nearestNow, kCandidates, a_job->standable, a_job->tooNear, a_job->seen,
                             a_job->offLeash, a_job->noMove);
            else if (a_job->flee)
                spdlog::info("[conceal] {:08X}: NO flee point farther from the foes within {:.0f}u ({} foe(s)) | {} candidates: standable "
                             "{}, beside a foe {}, not far enough from the foes {}, outside the leash {}, no real move {}",
                             a_job->follower, kRing[1], a_job->foes.size(), kCandidates, a_job->standable, a_job->tooNear, a_job->seen,
                             a_job->offLeash, a_job->noMove);
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

            if (a_job->flee && a_job->next == 0) {
                float nearest = (std::numeric_limits<float>::max)();
                for (RE::Actor* foe : foes) nearest = (std::min)(nearest, foe->GetPosition().GetDistance(me));
                a_job->nearestNow = nearest;
            }
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
                if (a_job->flee) {
                    // A flee needs no cover: a point standing clearly farther from every foe than he does now. `seen` counts
                    // the points that gain too little.
                    float nearest = (std::numeric_limits<float>::max)();
                    for (RE::Actor* foe : foes) nearest = (std::min)(nearest, foe->GetPosition().GetDistance(pt));
                    if (nearest < kMinFoeDist) { ++a_job->tooNear; continue; }
                    if (nearest < a_job->nearestNow + kFleeGain) { ++a_job->seen; continue; }
                    ++a_job->hidden;
                    if (!a_job->have || nearest > a_job->bestFoeDist) {   // the farthest from the foes
                        a_job->have        = true;
                        a_job->best        = pt;
                        a_job->bestCost    = cost;
                        a_job->bestFoeDist = nearest;
                    }
                    continue;
                }
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

    namespace {
        // WORKER. True when at least one listed foe is loaded and alive AND none of them has him (G2 Layer 1: the foe's own
        // group record of him is absent, lost or at detect level 0; no group at all is not "has him"). `a_anyYes` is set when a
        // foe does have him. Nothing is written.
        bool NoFoeDetects(RE::Actor* a_follower, const std::vector<RE::FormID>& a_foes, bool& a_anyYes) {
            a_anyYes = false;
            const std::uint32_t handle = a_follower->GetHandle().native_handle();
            int resolved = 0;
            for (const RE::FormID id : a_foes) {
                auto* foe = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!foe || foe->IsDead() || !foe->Is3DLoaded()) continue;
                ++resolved;
                if (Iaus::Hide::Verdict(Iaus::Hide::Read(foe, handle)) == Iaus::Tri::Yes) { a_anyYes = true; return false; }
            }
            return resolved > 0;
        }

        // WORKER. The foes to ask THIS lap (re-review F2 of ba1328c: the pick's list is frozen at pick time and holds 6 at
        // most): every foe his own combat group lists now, by the gather's own set tests (iaus/Candidates.cpp Gather: not
        // lost, not himself, not Harbinger's anchor, alive, enabled, loaded, hostile to him), with no cap, plus the pick's
        // foes (one he has lost track of can still have him). The follower's group lock is released before any foe is read
        // (NoFoeDetects takes each FOE's group lock, never nested).
        std::vector<RE::FormID> WatchList(RE::Actor* a_follower, const std::vector<RE::FormID>& a_picked) {
            std::vector<RE::FormID>      out = a_picked;
            std::vector<RE::ActorHandle> raw;
            if (auto* cc = a_follower->GetActorRuntimeData().combatController; cc && cc->combatGroup) {
                RE::BSReadLockGuard lk(cc->combatGroup->lock);
                for (const auto& t : cc->combatGroup->targets) {
                    if (t.flags.any(RE::CombatTarget::Flags::kTargetLost)) continue;
                    raw.push_back(t.targetHandle);
                }
            }
            for (const auto& h : raw) {
                auto  ptr = h.get();   // HOLD the NiPointer
                auto* foe = ptr.get();
                if (!foe || foe == a_follower) continue;
                const RE::FormID fid = foe->GetFormID();
                if (APMFBridge::IsSubstrateAnchor(fid)) continue;
                if (foe->IsDead() || foe->IsDisabled() || !foe->Is3DLoaded()) continue;
                if (!foe->IsHostileToActor(a_follower)) continue;
                if (std::find(out.begin(), out.end(), fid) == out.end()) out.push_back(fid);
            }
            return out;
        }
    }

    bool RoadOpen() {
        return Config::g_iausHideRoad.load(std::memory_order_relaxed) && APMFBridge::DisengageSupported() &&
               MainThread::IsInstalled();
    }

    bool FleeRunning(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_runs.find(a_follower);
        return it != g_runs.end() && it->second.phase != Phase::None && it->second.flee;
    }

    bool Prepare(RE::Actor* a_actor, const std::vector<RE::FormID>& a_foes, bool a_flee) {
        if (!a_actor || a_foes.empty() || !RoadOpen()) return false;
        const RE::FormID     a_follower = a_actor->GetFormID();
        const float          leash = Confidence::LeashRadius(a_actor);   // worker read, as the scheduler's own
        std::shared_ptr<Job> job;
        bool                 fresh = false;
        {
            std::scoped_lock lk(g_mx);
            if (const auto r = g_runs.find(a_follower); r != g_runs.end() && r->second.phase != Phase::None) return false;
            if (g_stances.contains(a_follower)) return false;   // he is already hidden and holding
            Pick&      p   = g_picks[a_follower];
            const auto now = Clock::now();
            fresh = p.valid && p.flee == a_flee && now - p.at < kFreshMs;
            if (!p.pending && now - p.askedAt >= kRepickMs) {
                p.pending = true;
                p.askedAt = now;
                job = std::make_shared<Job>();
                job->follower = a_follower;
                job->epoch    = g_epoch;
                job->leash    = leash;
                job->flee     = a_flee;
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
        bool             flee = false;
        std::vector<RE::FormID> foes;
        {
            std::scoped_lock lk(g_mx);
            if (const auto r = g_runs.find(id); r != g_runs.end() && r->second.phase != Phase::None) return "a hide run already stands";
            const auto p = g_picks.find(id);
            if (p == g_picks.end() || !p->second.valid || Clock::now() - p->second.at >= kFreshMs)
                return "no fresh concealment point (the pick found none, or its answer is older than 3 s)";
            pt   = p->second.point;
            flee = p->second.flee;
            foes = p->second.foes;
        }
        // The pick is consumed only once Harbinger has ACCEPTED the request (review of 1f59152 SEV-4: a refusal changed nothing).
        // The leg's ch.7 deny (marth 2026-11-11, Offense means hostile): a FLEE denies hostile offense only, so his own
        // heals land (Harbinger ABI >= 40; below it no ch.7 claim is filed, said once). A HIDE RUN denies attacks and every
        // cast, as it always did.
        const auto deny = flee ? APMFBridge::DisengageDeny::HostileOffense : APMFBridge::DisengageDeny::OffenseAndCast;
        if (const char* why = APMFBridge::RequestDisengage(a_follower, pt, kRadius, deny)) return why;
        {
            std::scoped_lock lk(g_mx);
            if (const auto p = g_picks.find(id); p != g_picks.end()) p->second.valid = false;   // one run per answer
            Run& r      = g_runs[id];
            r           = Run{};
            r.phase     = Phase::Running;
            r.point     = pt;
            r.startedAt = Clock::now();
            r.flee      = flee;
            r.foes      = std::move(foes);
        }
        a_desc = fmt::format("{} to a {} point {:.0f}u away ({:.0f},{:.0f},{:.0f})", flee ? "fleeing" : "disengaging",
                             flee ? "flee" : "concealment", a_follower->GetPosition().GetDistance(pt), pt.x, pt.y, pt.z);
        return nullptr;
    }

    bool Service(RE::Actor* a_follower) {
        if (!a_follower) return false;
        const RE::FormID id = a_follower->GetFormID();
        Run run;
        bool haveRun = false;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_runs.find(id);
            if (it != g_runs.end() && it->second.phase != Phase::None) { run = it->second; haveRun = true; }
        }
        if (!haveRun) {
            // No run: the hide hold's stance (a hidden run ended and left G2's hold in charge) is still watched.
            std::vector<RE::FormID> foes;
            {
                std::scoped_lock lk(g_mx);
                const auto st = g_stances.find(id);
                if (st == g_stances.end()) return false;
                foes = st->second.foes;
            }
            const char* why = nullptr;
            bool anyYes = false;
            if (!APMFBridge::InCombat(a_follower)) why = "his combat ended";
            else { NoFoeDetects(a_follower, WatchList(a_follower, foes), anyYes); if (anyYes) why = "a foe detects him again (revealed)"; }
            if (why) {
                { std::scoped_lock lk(g_mx); g_stances.erase(id); }
                APMFBridge::ReleaseSneakHide(id, why);
                spdlog::info("[conceal] {:08X}: the hide hold's stance is released: {}", id, why);
            }
            return false;   // G2's hold row / the blow from stealth belong to the IAUS: no scan is skipped
        }

        const auto now = Clock::now();
        // keepStance: the run ended because he is HIDDEN, so the G1 claim stays for G2's hold row (released on the blow, a
        // reveal, the fight's end, dismissal, death or a load). Any other end drops it. The cooldown counts from HERE.
        auto end = [&](const char* a_why, bool a_failure, bool a_keepStance) {
            {
                std::scoped_lock lk(g_mx);
                g_runs.erase(id);
                if (a_keepStance) g_stances[id].foes = run.foes;
            }
            APMFBridge::ReleaseDisengage(id, a_why);
            if (!a_keepStance && !run.flee) APMFBridge::ReleaseSneakHide(id, a_why);
            Iaus::Engine::NoteDisengageEnded(id);
            if (a_failure) spdlog::warn("[conceal] {:08X}: {} run FAILED: {}. Not retried until its cooldown (counted from now) is over.",
                                        id, run.flee ? "flee" : "hide", a_why);
            else spdlog::info("[conceal] {:08X}: {} run over: {}", id, run.flee ? "flee" : "hide", a_why);
            return false;   // the run no longer holds him: the scan runs this lap
        };

        // The engine reports him out of his OWN combat (a Harbinger substrate is not a fight). Harbinger ends the ch.24 claim
        // on that itself (CombatEnded), so the run has no road left. Not a failure, and said as what it is.
        if (!APMFBridge::InCombat(a_follower))
            return end(run.phase == Phase::Hiding ? "the engine reports him out of combat at the point (no foe holds him)"
                                                  : "the engine reports him out of combat on the way", false, false);

        if (run.flee) {
            // NON-SNEAK FLEE (marth 2026-11-11: "for anyone else, they would try to heal asap during the flee"). The IAUS keeps
            // running (this returns false every lap): offense is not generated and his own heals are boosted while the run
            // stands (iaus/Generate.cpp, Snapshot::fleeRun). It ends when he is healed past the retreat's health line, when the
            // drive arrives, or when it fails.
            // the same max as the IAUS pool read (iaus/Candidates.cpp ReadPool): permanent + temporary modifier
            const float hpMax = a_follower->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth) +
                                a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth);
            const float hpPct = hpMax > 0.0f ? Health(a_follower) / hpMax : 1.0f;
            if (hpPct > Iaus::Disengage::kFleeHealthLine)
                return end("healed past the retreat's health line", false, false);
            std::string why;
            const auto  st = APMFBridge::ServiceDisengage(id, why);
            if (st == APMFBridge::DisengageState::None) return end("the drive is gone (released elsewhere)", true, false);
            if (st == APMFBridge::DisengageState::Failed) return end(why.c_str(), true, false);
            if (st == APMFBridge::DisengageState::Arrived) return end("arrived at the point away from the foes", false, false);
            if (now - run.startedAt > kRunMaxMs) return end("he did not reach the point in 10 s", true, false);
            return false;
        }

        // HIDE RUN (a sneak class). It ends AS SOON AS the engine says no foe detects him (G2 Layer 1's verdict), on the way or
        // at the point: the purpose is to become hidden, and then G2's hold / blow from stealth takes over.
        // Read over his CURRENT foes plus the pick's (WatchList), and only after kHiddenReads consecutive laps that say so.
        {
            bool       anyYes = false;
            const bool none   = NoFoeDetects(a_follower, WatchList(a_follower, run.foes), anyYes);
            const int  reads  = none ? run.hiddenReads + 1 : 0;
            if (reads != run.hiddenReads) {
                run.hiddenReads = reads;
                std::scoped_lock lk(g_mx);
                if (const auto it = g_runs.find(id); it != g_runs.end()) it->second.hiddenReads = reads;
            }
            if (reads >= kHiddenReads) {
                if (const char* why = APMFBridge::HoldSneakForHide(a_follower))
                    spdlog::warn("[conceal] {:08X}: hidden, but NOT sneaking: {}. G2's hold row needs the stance.", id, why);
                return end(run.phase == Phase::Hiding ? "hidden at the point: no foe detects him (G2's verdict)"
                                                      : "hidden on the way: no foe detects him (G2's verdict)", false, true);
            }
        }

        if (run.phase == Phase::Running) {
            std::string why;
            const auto  st = APMFBridge::ServiceDisengage(id, why);
            if (st == APMFBridge::DisengageState::None) return end("the drive is gone (released elsewhere)", true, false);
            if (st == APMFBridge::DisengageState::Failed) return end(why.c_str(), true, false);
            if (st == APMFBridge::DisengageState::Running) {
                if (now - run.startedAt > kRunMaxMs) return end("he did not reach the point in 10 s", true, false);
                return true;
            }
            // Arrived: the crouch (the G1 stance claim, held for the hide) and the hold start now.
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
            spdlog::info("[conceal] {:08X}: at the concealment point after {:.1f} s: crouching (the G1 stance claim, held for the "
                         "hide) and holding until the engine loses him (cap {} s)",
                         id, std::chrono::duration<float>(now - run.startedAt).count(), kHideHoldMs.count() / 1000);
        }

        // Hiding. The stance claim, held through the hide (apmf/SneakClaims.cpp HoldSneakForHide; it overrides bSneakInCombat for
        // this owner and is not aged by table laps). Idempotent each lap.
        if (const char* why = APMFBridge::HoldSneakForHide(a_follower); why && !run.sneakNoteLogged) {
            {
                std::scoped_lock lk(g_mx);
                if (const auto it = g_runs.find(id); it != g_runs.end()) it->second.sneakNoteLogged = true;
            }
            spdlog::warn("[conceal] {:08X}: at the point but NOT sneaking: {}. He holds there standing; the engine's own rules "
                         "decide whether a foe loses him.", id, why);
        }
        if (Health(a_follower) < run.hpAtArrival - 1.0f) return end("he was hit at the point (a foe found him)", false, false);
        if (now - run.arrivedAt > kHideHoldMs)
            return end("the hold CAP (12 s) was hit and the engine still lists a foe that detects him", true, false);
        return true;
    }

    void NoteBlow(RE::FormID a_follower) {
        {
            std::scoped_lock lk(g_mx);
            if (g_stances.erase(a_follower) == 0) return;
        }
        APMFBridge::ReleaseSneakHide(a_follower, "the blow from stealth");
        spdlog::info("[conceal] {:08X}: the blow from stealth is picked: the hide hold's stance is released", a_follower);
    }

    void Release(RE::FormID a_follower, const char* a_why) {
        bool had = false, stance = false;
        {
            std::scoped_lock lk(g_mx);
            had    = g_runs.erase(a_follower) != 0;
            stance = g_stances.erase(a_follower) != 0;
            g_picks.erase(a_follower);
        }
        APMFBridge::ReleaseDisengage(a_follower, a_why);   // idempotent (a drive Begin filed and the run lost track of)
        APMFBridge::ReleaseSneakHide(a_follower, a_why);   // idempotent
        if (had) Iaus::Engine::NoteDisengageEnded(a_follower);   // the cooldown counts from the end, however it ended
        if (had) spdlog::info("[conceal] {:08X}: hide run released ({})", a_follower, a_why ? a_why : "");
        if (stance) spdlog::info("[conceal] {:08X}: the hide hold's stance is released ({})", a_follower, a_why ? a_why : "");
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mx);
        ++g_epoch;
        g_picks.clear();
        g_runs.clear();
        g_stances.clear();
    }
}
