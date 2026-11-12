#include "PCH.h"
#include "Concealment.h"

#include "Confidence.h"          // LeashRadius: the pick stays inside the follower's leash
#include "Config.h"
#include "Followers.h"          // IsTrackedFast: the any-thread FormID mirror the hit sink filters on (#74)
#include "MainThread.h"
#include "Sightline.h"           // PointSeenNow: MFO's own ray from a foe's eye to a point
#include "apmf/APMFBridge.h"     // the disengage drive (H3), FindStandPoint, the G1 stance claim
#include "cast/Actuation.h"      // OwnCastInFlight: his own cast is in a live caster state (the one in-flight definition)
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
//   kBlowCapMs            THE BLOW FROM STEALTH (review P1-G3 of 938f45f). The stance is HELD from the pick until the blow
//                         RESOLVES: the engine reads "attacker is sneaking" when the hit lands, and a swing needs 0.3 s or
//                         more from the pick to its hit. Resolved = his first hit on an actor (TESHitEvent, cause = him), or
//                         his attack state / own cast seen live after the pick and then back to none. The hold has NO
//                         timer from the pick (the walk to the target has no fixed length: before the swing shows, the
//                         stance is exactly the hide hold it was, with the reveal road intact). This cap counts only from
//                         the lap the swing / cast was FIRST SEEN live, so it is a ceiling on one engine action, never
//                         an expiry of a hold in use (principle 9): a melee swing or a bash is over in under 2 s, a drawn
//                         bow or a charged cast is held a few seconds by the combat AI. 6 s is above those (chosen, not
//                         measured: hitting it is logged as a warning so a field run can correct it).
//   kBlowFlightMs         a shot's or a cast's projectile is in the air after the attack state is back to none. The
//                         stance is kept this long after a RANGED or CAST blow leaves the hand, unless the hit event
//                         comes first (it normally does). 1.5 s covers ~3000 u of arrow flight (chosen). A melee blow
//                         lands inside the swing, so it has no tail.
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
        constexpr std::chrono::milliseconds kBlowCapMs{ 6000 };
        constexpr std::chrono::milliseconds kBlowFlightMs{ 1500 };
        // BATCH I (the BackOff and Step kinds; the header has the contract). Sized from:
        //   kStepRing / kStepOff   the Step's candidates: 5 bearings (the way to the foe, +-25, +-50 degrees) on two short rings. A step is
        //                          one hop toward a foe (the next lap picks the next), not a route, so the rings are shorter than the hide's.
        //   kStepGain              a step point must stand at least this much nearer the foe than he does (a smaller gain is no step).
        //   kBackOffMaxMs          a back-off is 320 to 600 units at the engine's run speed (under 2 s): 5 s is a path around furniture; past it
        //                          the run is a FAILURE, said out loud (principle 7).
        //   kStepRunMaxMs          a quiet step at the sneak gait (x0.55 of a run, the lab's number) over 420 units: 8 s with margin.
        constexpr int   kStepOffCount = 5;
        constexpr float kStepOff[kStepOffCount] = { 0.0f, 0.4363f, -0.4363f, 0.8727f, -0.8727f };   // radians: 0, +-25, +-50 degrees
        constexpr float kStepRing[2] = { 200.0f, 420.0f };
        constexpr int   kStepCandidates = kStepOffCount * 2;
        constexpr float kStepGain = 100.0f;
        constexpr std::chrono::milliseconds kBackOffMaxMs{ 5000 };
        constexpr std::chrono::milliseconds kStepRunMaxMs{ 8000 };

        const char* KindName(Kind a_k) {
            switch (a_k) {
            case Kind::Hide:    return "hide";
            case Kind::Flee:    return "flee";
            case Kind::BackOff: return "back-off";
            case Kind::Step:    return "step";
            }
            return "?";
        }

        struct Pick {
            RE::NiPoint3      point{};
            Clock::time_point at{};        // when the answer landed
            Clock::time_point askedAt{};
            bool              valid = false;
            bool              pending = false;
            bool              flee = false;              // the answer is for a flee (a point away from the foes), not a hide
            Kind              kind = Kind::Hide;         // batch I: the answer is for exactly this kind (flee == Flee or BackOff: a point away)
            RE::FormID        target = 0;                // batch I: a Step's foe
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
            Kind              kind = Kind::Hide;         // batch I: Hide / Flee (flee above) / BackOff / Step
            int               hiddenReads = 0;           // consecutive laps on which no foe detected him (the hide run ends at kHiddenReads)
            std::vector<RE::FormID> foes;                // the pick's foes; every lap reads these AND his current foes (WatchList)
        };
        struct Stance {                                  // the hide hold after a hidden run: the G1 claim stays until a blow RESOLVES / reveal / end
            std::vector<RE::FormID> foes;
            // The blow from stealth (NoteBlow arms it on the pick; Service resolves it; the hit sink marks the hit).
            bool              armed = false;             // a blow was picked: the stance is held until it resolves
            bool              live = false;              // his swing / shot / cast was seen in progress since the pick
            bool              ranged = false;            // ... and it was a bow state or a cast (a projectile may still fly)
            bool              idle = false;              // seen live, and now back to none (idleAt = the first such lap)
            bool              hit = false;               // SINK: his hit on an actor landed since the pick
            std::uint8_t      hitFlags = 0;              // SINK: TESHitEvent::Flag bits of that hit
            RE::FormID        hitTarget = 0;             // SINK
            Clock::time_point armedAt{}, liveAt{}, idleAt{}, hitAt{};
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
            Kind                    kind = Kind::Hide;     // batch I: BackOff picks like a flee (flee is true for it); Step has its own candidates
            RE::FormID              target = 0;            // a Step's foe
            bool                    bow = false;           // a Step for a line of fire (else for a blow's reach)
            float                   targetNow = 0.0f;      // a Step: his distance to the target when the pick began
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
                p.kind    = a_job->kind;
                p.target  = a_job->target;
                p.foes    = a_job->foes;
            }
            if (a_abort)
                spdlog::info("[conceal] {:08X}: pick dropped ({})", a_job->follower, a_abort);
            else if (a_job->kind == Kind::Step)
                spdlog::info("[conceal] {:08X}: STEP {} toward {:08X} ({}): {:.0f},{:.0f},{:.0f} ({:.0f}u from him, {:.0f}u from the foe; he stands {:.0f}u "
                             "from it) | {} candidates: standable {}, no real move {}, outside the leash {}, not nearer the foe {}, no line to "
                             "the foe {}, ray unknown {}, beside another foe {}",
                             a_job->follower, a_job->have ? "point" : "NONE", a_job->target, a_job->bow ? "line of fire" : "reach",
                             a_job->best.x, a_job->best.y, a_job->best.z, a_job->bestCost, a_job->bestFoeDist, a_job->targetNow,
                             kStepCandidates, a_job->standable, a_job->noMove, a_job->offLeash, a_job->seen, a_job->hidden, a_job->unknown,
                             a_job->tooNear);
            else if (a_job->have && a_job->kind == Kind::BackOff)
                spdlog::info("[conceal] {:08X}: BACK-OFF point {:.0f},{:.0f},{:.0f} ({:.0f}u from him), {:.0f}u from the nearest of {} foe(s) "
                             "(he stands {:.0f}u from it now)",
                             a_job->follower, a_job->best.x, a_job->best.y, a_job->best.z, a_job->bestCost, a_job->bestFoeDist,
                             a_job->foes.size(), a_job->nearestNow);
            else if (a_job->kind == Kind::BackOff)
                spdlog::info("[conceal] {:08X}: NO back-off point farther from the {} foe(s) within {:.0f}u | {} candidates: standable {}, "
                             "beside a foe {}, not far enough from the foes {}, outside the leash {}, no real move {}",
                             a_job->follower, a_job->foes.size(), kRing[1], kCandidates, a_job->standable, a_job->tooNear, a_job->seen,
                             a_job->offLeash, a_job->noMove);
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

            if (a_job->kind == Kind::Step) {
                // THE HIDDEN STEP (batch I item 5): candidates toward the foe the step is for.
                auto* tf = RE::TESForm::LookupByID<RE::Actor>(a_job->target);
                if (!tf || tf->IsDead() || !tf->Is3DLoaded()) return Finish(a_job, "the step's foe is gone");
                const RE::NiPoint3 tp = tf->GetPosition();
                if (a_job->next == 0) a_job->targetNow = me.GetDistance(tp);
                const float base = std::atan2(tp.x - me.x, tp.y - me.y);   // FindStandPoint's heading: radians, 0 = +Y, clockwise
                for (int n = 0; n < kPerFrame && a_job->next < kStepCandidates; ++n, ++a_job->next) {
                    const int   i       = a_job->next;
                    const float heading = base + kStepOff[i % kStepOffCount];
                    const float dist    = kStepRing[i / kStepOffCount];
                    RE::NiPoint3  pt{};
                    std::uint32_t status = 0;
                    if (!APMFBridge::FindStandPoint(a_job->follower, heading, dist, kClearance, pt, status)) continue;
                    ++a_job->standable;
                    const float cost = me.GetDistance(pt);
                    if (cost < kMinMove) { ++a_job->noMove; continue; }
                    if (pc && leash > 0.0f && pc->GetPosition().GetDistance(pt) > leash - kLeashMargin) { ++a_job->offLeash; continue; }
                    const float toTarget = pt.GetDistance(tp);
                    if (toTarget > a_job->targetNow - kStepGain) { ++a_job->seen; continue; }   // not a step toward the foe
                    bool beside = false;
                    for (RE::Actor* foe : foes)
                        if (foe != tf && foe->GetPosition().GetDistance(pt) < kMinFoeDist) { beside = true; break; }
                    if (beside) { ++a_job->tooNear; continue; }
                    if (a_job->bow) {
                        // A line of fire: the foe's eye can reach the point (MFO's own ray, clear of world geometry). Unknown is never taken.
                        const auto ps = Sightline::PointSeenNow(tf, pt, height);
                        if (ps == Sightline::PointSight::Unavailable) { ++a_job->unknown; continue; }
                        if (ps != Sightline::PointSight::Seen) { ++a_job->hidden; continue; }
                        if (!a_job->have || cost < a_job->bestCost) {   // the shortest walk that gives a line
                            a_job->have = true; a_job->best = pt; a_job->bestCost = cost; a_job->bestFoeDist = toTarget;
                        }
                    } else if (!a_job->have || toTarget < a_job->bestFoeDist) {   // a blade: the point nearest the foe
                        a_job->have = true; a_job->best = pt; a_job->bestCost = cost; a_job->bestFoeDist = toTarget;
                    }
                }
                if (a_job->next >= kStepCandidates) return Finish(a_job, nullptr);
                MainThread::Post([a_job]() { PickStep(a_job); });
                return;
            }
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

    namespace {
        // WORKER, once per lap of a follower whose hide hold's stance stands and who is in combat. The blow from stealth:
        // armed on the pick (NoteBlow), resolved HERE. Returns true when the stance was released by the blow's end or when
        // a swing / shot / cast in progress holds it this lap (the reveal road is not read then: the engine applies the
        // sneak multiplier against the TARGET's detection at the hit, and standing him up mid-swing because another foe
        // saw him would throw the blow away). Returns false when no blow is armed or none has started yet: the caller's
        // reveal road decides, exactly as before the pick.
        // The attack state and the caster reads are plain loads on the worker, as iaus/Fills.cpp and cast/Hands.cpp make.
        bool ServiceBlow(RE::Actor* a_follower, RE::FormID a_id) {
            const auto* as    = a_follower->AsActorState();
            const auto  atk   = as ? as->GetAttackState() : RE::ATTACK_STATE_ENUM::kNone;
            const bool  cast  = Actuation::OwnCastInFlight(a_follower);
            const bool  bow   = atk >= RE::ATTACK_STATE_ENUM::kBowDraw;
            const bool  live  = atk != RE::ATTACK_STATE_ENUM::kNone || cast;
            const auto  now   = Clock::now();
            const char* why   = nullptr;
            bool        warn = false, firstLive = false;
            Stance      st;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_stances.find(a_id);
                if (it == g_stances.end() || !it->second.armed) return false;
                Stance& s = it->second;
                if (live) {
                    if (!s.live) { s.live = true; s.liveAt = now; firstLive = true; }
                    s.ranged = s.ranged || bow || cast;
                    s.idle   = false;
                } else if (s.live && !s.idle) {
                    s.idle = true; s.idleAt = now;
                }
                if (s.hit) why = "his hit landed";
                else if (live && now - s.liveAt > kBlowCapMs) { why = "the blow's CAP was hit with his attack or cast still live and no hit event"; warn = true; }
                else if (s.live && s.idle && (!s.ranged || now - s.idleAt > kBlowFlightMs))
                    why = s.ranged ? "his shot or cast ended and no hit event came inside the flight time (a miss)"
                                   : "his swing ended with no hit event (a miss or a block the engine did not report)";
                st = s;
                if (why) g_stances.erase(it);
                else if (!s.live) return false;   // picked, not started (he is closing in): the hold is as it was
            }
            const auto ms = [&](Clock::time_point a_from, Clock::time_point a_to) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(a_to - a_from).count();
            };
            if (!why) {
                if (firstLive)
                    spdlog::info("[conceal] {:08X}: the blow from stealth is under way {} ms after the pick (attack state {}, own cast "
                                 "{}): the stance is kept through it, sneaking now = {}",
                                 a_id, ms(st.armedAt, now), static_cast<int>(atk), cast ? "live" : "none",
                                 a_follower->IsSneaking() ? "yes" : "NO");
                return true;
            }
            APMFBridge::ReleaseSneakHide(a_id, "the blow from stealth resolved");
            if (st.hit) {
                using F = RE::TESHitEvent::Flag;
                const auto f = st.hitFlags;
                spdlog::info("[conceal] {:08X}: the blow from stealth RESOLVED: his hit landed on {:08X} {} ms after the pick with the "
                             "stance held (sneak attack flag {}{}{}{}). The hide hold's stance is released now.",
                             a_id, st.hitTarget, ms(st.armedAt, st.hitAt),
                             (f & static_cast<std::uint8_t>(F::kSneakAttack)) ? "SET" : "NOT set",
                             (f & static_cast<std::uint8_t>(F::kPowerAttack)) ? ", power" : "",
                             (f & static_cast<std::uint8_t>(F::kBashAttack)) ? ", bash" : "",
                             (f & static_cast<std::uint8_t>(F::kHitBlocked)) ? ", blocked" : "");
            } else if (warn) {
                spdlog::warn("[conceal] {:08X}: the blow from stealth did NOT resolve: {} ({} ms since it was first seen live, cap {} "
                             "ms, attack state {}, own cast {}). The hide hold's stance is released.",
                             a_id, why, ms(st.liveAt, now), kBlowCapMs.count(), static_cast<int>(atk), cast ? "live" : "none");
            } else {
                spdlog::info("[conceal] {:08X}: the blow from stealth RESOLVED without a hit: {} ({} ms after the pick, stance held "
                             "until now). The hide hold's stance is released.", a_id, why, ms(st.armedAt, now));
            }
            return true;
        }

        // The engine posts TESHitEvent on whatever thread the hit resolves on. LEAF: a FormID compare under g_mx, no engine
        // call, no other lock, no follower list (Followers::IsTrackedFast is the any-thread mirror, #74).
        class BlowHitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
        public:
            static BlowHitSink* GetSingleton() { static BlowHitSink s; return &s; }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_ev, RE::BSTEventSource<RE::TESHitEvent>*) override {
                if (!a_ev || !a_ev->target || !a_ev->cause) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID cause = a_ev->cause->GetFormID();
                if (!Followers::IsTrackedFast(cause)) return RE::BSEventNotifyControl::kContinue;
                if (!a_ev->target->As<RE::Actor>()) return RE::BSEventNotifyControl::kContinue;   // a hit on an object is not the blow
                const RE::FormID target = a_ev->target->GetFormID();
                const auto       flags  = static_cast<std::uint8_t>(a_ev->flags.underlying());
                std::scoped_lock lk(g_mx);
                const auto it = g_stances.find(cause);
                if (it == g_stances.end() || !it->second.armed || it->second.hit) return RE::BSEventNotifyControl::kContinue;
                it->second.hit       = true;
                it->second.hitFlags  = flags;
                it->second.hitTarget = target;
                it->second.hitAt     = Clock::now();
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void RegisterHitSink() {
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESHitEvent>(BlowHitSink::GetSingleton());
            spdlog::info("[conceal] hit sink registered (the blow from stealth resolves on his hit; it reads only a held stance)");
        }
    }

    bool RoadOpen() {
        return Config::g_iausHideRoad.load(std::memory_order_relaxed) && APMFBridge::DisengageSupported() &&
               MainThread::IsInstalled();
    }

    bool FleeRunning(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_runs.find(a_follower);
        return it != g_runs.end() && it->second.phase != Phase::None && it->second.kind == Kind::Flee;   // a BackOff is not a flee: offense stays on
    }

    bool Prepare(RE::Actor* a_actor, const std::vector<RE::FormID>& a_foes, bool a_flee) {
        return PrepareKind(a_actor, a_foes, a_flee ? Kind::Flee : Kind::Hide);
    }

    bool PrepareKind(RE::Actor* a_actor, const std::vector<RE::FormID>& a_foes, Kind a_kind, RE::FormID a_target, bool a_bow) {
        if (!a_actor || a_foes.empty() || !RoadOpen()) return false;
        const bool a_flee = a_kind == Kind::Flee || a_kind == Kind::BackOff;   // the pick shape: a point away from the foes, no cover
        const RE::FormID     a_follower = a_actor->GetFormID();
        const float          leash = Confidence::LeashRadius(a_actor);   // worker read, as the scheduler's own
        std::shared_ptr<Job> job;
        bool                 fresh = false;
        {
            std::scoped_lock lk(g_mx);
            if (const auto r = g_runs.find(a_follower); r != g_runs.end() && r->second.phase != Phase::None) return false;
            // He is already hidden and holding: no hide run. A BackOff or a Step is exactly what a hold may want (batch I).
            if ((a_kind == Kind::Hide || a_kind == Kind::Flee) && g_stances.contains(a_follower)) return false;
            Pick&      p   = g_picks[a_follower];
            const auto now = Clock::now();
            fresh = p.valid && p.kind == a_kind && (a_kind != Kind::Step || p.target == a_target) && now - p.at < kFreshMs;
            if (!p.pending && now - p.askedAt >= kRepickMs) {
                p.pending = true;
                p.askedAt = now;
                job = std::make_shared<Job>();
                job->follower = a_follower;
                job->epoch    = g_epoch;
                job->leash    = leash;
                job->flee     = a_flee;
                job->kind     = a_kind;
                job->target   = a_target;
                job->bow      = a_bow;
                job->foes.assign(a_foes.begin(), a_foes.begin() + static_cast<std::ptrdiff_t>((std::min)(a_foes.size(), kMaxFoes)));
            }
        }
        if (job) MainThread::Post([job]() { PickStep(job); });   // outside our lock (MainThread has its own queue mutex)
        return fresh;
    }

    const char* Begin(RE::Actor* a_follower, std::string& a_desc, Kind a_want) {
        if (!a_follower) return "no follower";
        if (!RoadOpen()) return "no hide road ([Debug] bIausHideRoad off, Harbinger absent or older than ABI 39, or no main-thread pump)";
        const RE::FormID id = a_follower->GetFormID();
        RE::NiPoint3     pt{};
        bool             flee = false;
        Kind             kind = Kind::Hide;
        std::vector<RE::FormID> foes;
        {
            std::scoped_lock lk(g_mx);
            if (const auto r = g_runs.find(id); r != g_runs.end() && r->second.phase != Phase::None) return "a hide run already stands";
            const auto p = g_picks.find(id);
            if (p == g_picks.end() || !p->second.valid || Clock::now() - p->second.at >= kFreshMs)
                return "no fresh concealment point (the pick found none, or its answer is older than 3 s)";
            // act.disengage asks for ONE kind (0 = the hide run or the flee, whichever the pick is for): a pick for another kind is not it.
            const Kind pk = p->second.kind;
            if (a_want == Kind::Hide ? (pk != Kind::Hide && pk != Kind::Flee) : pk != a_want)
                return "the fresh point is for another kind of run";
            pt   = p->second.point;
            flee = pk == Kind::Flee;
            kind = pk;
            foes = p->second.foes;
        }
        // The pick is consumed only once Harbinger has ACCEPTED the request (review of 1f59152 SEV-4: a refusal changed nothing).
        // The leg's ch.7 deny (marth 2026-11-11, Offense means hostile): a FLEE denies hostile offense only, so his own
        // heals land (Harbinger ABI >= 40; below it no ch.7 claim is filed, said once). A HIDE RUN denies attacks and every
        // cast, as it always did.
        // A BackOff denies nothing (he fights while he moves); a Step denies every attack and cast (quiet, never a reveal by design).
        const auto deny = kind == Kind::BackOff ? APMFBridge::DisengageDeny::None
                        : flee ? APMFBridge::DisengageDeny::HostileOffense : APMFBridge::DisengageDeny::OffenseAndCast;
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
            r.kind      = kind;
            r.foes      = std::move(foes);
        }
        a_desc = fmt::format("{} to a {} point {:.0f}u away ({:.0f},{:.0f},{:.0f})",
                             kind == Kind::BackOff ? "backing off" : kind == Kind::Step ? "stepping hidden" : flee ? "fleeing" : "disengaging",
                             kind == Kind::BackOff ? "back-off" : kind == Kind::Step ? "step" : flee ? "flee" : "concealment",
                             a_follower->GetPosition().GetDistance(pt), pt.x, pt.y, pt.z);
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
            if (!APMFBridge::InCombat(a_follower)) {
                { std::scoped_lock lk(g_mx); g_stances.erase(id); }
                APMFBridge::ReleaseSneakHide(id, "his combat ended");
                spdlog::info("[conceal] {:08X}: the hide hold's stance is released: his combat ended", id);
                return false;
            }
            // THE BLOW FROM STEALTH (armed by NoteBlow on the pick). The stance stays through the swing and its hit; it is
            // released when the blow resolves. true = this lap is decided (held, or released here).
            if (ServiceBlow(a_follower, id)) return false;
            bool anyYes = false;
            NoFoeDetects(a_follower, WatchList(a_follower, foes), anyYes);
            if (anyYes) {
                bool had = false;
                { std::scoped_lock lk(g_mx); had = g_stances.erase(id) != 0; }
                if (had) {
                    APMFBridge::ReleaseSneakHide(id, "a foe detects him again (revealed)");
                    spdlog::info("[conceal] {:08X}: the hide hold's stance is released: a foe detects him again (revealed)", id);
                }
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
            const bool shortRun = run.kind == Kind::BackOff || run.kind == Kind::Step;   // batch I: their families pace themselves (Tuning)
            if (!shortRun) Iaus::Engine::NoteDisengageEnded(id);
            if (a_failure) spdlog::warn("[conceal] {:08X}: {} run FAILED: {}. Not retried until its cooldown (counted from now) is over.",
                                        id, KindName(run.kind), a_why);
            else spdlog::info("[conceal] {:08X}: {} run over: {}", id, KindName(run.kind), a_why);
            return false;   // the run no longer holds him: the scan runs this lap
        };

        // The engine reports him out of his OWN combat (a Harbinger substrate is not a fight). Harbinger ends the ch.24 claim
        // on that itself (CombatEnded), so the run has no road left. Not a failure, and said as what it is.
        if (!APMFBridge::InCombat(a_follower))
            return end(run.phase == Phase::Hiding ? "the engine reports him out of combat at the point (no foe holds him)"
                                                  : "the engine reports him out of combat on the way", false, false);

        // BATCH I: a BACK-OFF. He keeps fighting while he moves (no ch.7 deny, the scan is NOT skipped: this returns false every lap, so the
        // IAUS keeps scoring and casts or swings on the way). It ends on arrival, on the drive's failure, or at the cap.
        if (run.kind == Kind::BackOff) {
            std::string why;
            const auto  st = APMFBridge::ServiceDisengage(id, why);
            if (st == APMFBridge::DisengageState::None) return end("the drive is gone (released elsewhere)", true, false);
            if (st == APMFBridge::DisengageState::Failed) return end(why.c_str(), true, false);
            if (st == APMFBridge::DisengageState::Arrived) return end("backed off to the point; he fights on from there", false, false);
            if (now - run.startedAt > kBackOffMaxMs) return end("he did not reach the point in 5 s", true, false);
            return false;
        }

        // BATCH I: the HIDDEN STEP (gap G18). Quiet (the stance claim is held each lap, the drive denies every attack and cast) and
        // never a reveal by design: the moment the engine says any foe has him, the step stops (the engine's verdict, not ours). On
        // arrival the stance stays for G2's hold row, which then strikes (a foe in reach / in sight) or steps again.
        if (run.kind == Kind::Step) {
            bool       anyYes = false;
            NoFoeDetects(a_follower, WatchList(a_follower, run.foes), anyYes);
            if (anyYes) return end("a foe detects him: the step stops (revealed; the engine's verdict)", false, false);
            if (const char* why = APMFBridge::HoldSneakForHide(a_follower); why && !run.sneakNoteLogged) {
                {
                    std::scoped_lock lk(g_mx);
                    if (const auto it = g_runs.find(id); it != g_runs.end()) it->second.sneakNoteLogged = true;
                }
                spdlog::warn("[conceal] {:08X}: stepping but NOT sneaking: {}. The engine's own rules decide whether a foe notices.", id, why);
            }
            std::string why;
            const auto  st = APMFBridge::ServiceDisengage(id, why);
            if (st == APMFBridge::DisengageState::None) return end("the drive is gone (released elsewhere)", true, false);
            if (st == APMFBridge::DisengageState::Failed) return end(why.c_str(), true, false);
            if (st == APMFBridge::DisengageState::Arrived) return end("arrived: hidden at the new spot, the hold row looks again", false, true);
            if (now - run.startedAt > kStepRunMaxMs) return end("he did not reach the step's point in 8 s", true, false);
            return true;
        }

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
            const auto it = g_stances.find(a_follower);
            if (it == g_stances.end() || it->second.armed) return;   // no stance, or this blow is already armed (logged once)
            it->second.armed   = true;
            it->second.armedAt = Clock::now();
        }
        // Review P1-G3 of 938f45f: NOT released here. The engine reads "the attacker is sneaking" when the hit LANDS, and the
        // release reaches SneakStop about a frame later. Service (ServiceBlow) releases it when the blow resolves.
        spdlog::info("[conceal] {:08X}: the blow from stealth is picked: the hide hold's stance is KEPT until the blow resolves "
                     "(his hit, or his swing / shot / cast ending)", a_follower);
    }

    void Release(RE::FormID a_follower, const char* a_why) {
        bool had = false, stance = false, shortRun = false;
        {
            std::scoped_lock lk(g_mx);
            if (const auto r = g_runs.find(a_follower); r != g_runs.end())
                shortRun = r->second.kind == Kind::BackOff || r->second.kind == Kind::Step;   // batch I: no hide cooldown stamp
            had    = g_runs.erase(a_follower) != 0;
            stance = g_stances.erase(a_follower) != 0;
            g_picks.erase(a_follower);
        }
        APMFBridge::ReleaseDisengage(a_follower, a_why);   // idempotent (a drive Begin filed and the run lost track of)
        APMFBridge::ReleaseSneakHide(a_follower, a_why);   // idempotent
        if (had && !shortRun) Iaus::Engine::NoteDisengageEnded(a_follower);   // the cooldown counts from the end, however it ended
        if (had) spdlog::info("[conceal] {:08X}: {} released ({})", a_follower, shortRun ? "short run" : "hide run", a_why ? a_why : "");
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
