#include "PCH.h"
#include "Sightline.h"
#include "MainThread.h"
#include "Followers.h"   // TeammateInFireLine walks the maintained party list
#include <limits>        // SegDist's +inf sentinel -- NOT in the PCH (the v1.0.8/9 CI lesson)

namespace MFO::Sightline {

    namespace {

        using Clock = std::chrono::steady_clock;

        // A verdict is trusted this long, then reads as Unknown. ~5 evaluator
        // ticks: long enough that the 133 ms cadence always finds a warm entry
        // while the pump keeps refreshing, short enough that a foe stepping
        // through a doorway is not "occluded" for a whole fight.
        constexpr float kFreshSeconds = 1.0f;

        // Per-viewer floor between Post()s. PickFoe runs once per RULE per
        // tick, so one follower with five foe selectors would otherwise queue
        // five identical measurement batches into the same frame.
        constexpr float kRepostSeconds = 0.3f;

        struct Entry {
            bool              los = false;
            Clock::time_point at{};
        };

        // Written by the MAIN thread (the measurement), read by the WORKER
        // (the evaluator) -- cross-thread by design, so a real lock (#4).
        // This mutex is a LEAF: nothing is called while holding it, so it can
        // never participate in an inversion with MainThread's queue mutex or
        // the combat-group lock the caller may be inside.
        std::mutex g_mx;
        std::unordered_map<std::uint64_t, Entry> g_cache;
        std::unordered_map<RE::FormID, Clock::time_point> g_lastPost;

        constexpr std::uint64_t Key(RE::FormID a_viewer, RE::FormID a_target) {
            return (static_cast<std::uint64_t>(a_viewer) << 32) | a_target;
        }

        float Since(Clock::time_point a_t) {
            if (a_t.time_since_epoch().count() == 0) return 1.0e9f;
            return std::chrono::duration<float>(Clock::now() - a_t).count();
        }

        // MAIN THREAD ONLY -- the actual raycast. Resolves ids fresh (a handle
        // captured on the worker could be a different actor by the time the
        // frame runs it) and refuses anything without loaded 3D: a raycast
        // against an unloaded ref answers nothing and asks the havok world
        // about geometry that is not there.
        void Measure(RE::FormID a_viewer, const std::vector<RE::FormID>& a_targets) {
            auto* vf = RE::TESForm::LookupByID<RE::Actor>(a_viewer);
            if (!vf || vf->IsDead() || !vf->Is3DLoaded()) return;

            for (const auto tid : a_targets) {
                auto* tf = RE::TESForm::LookupByID<RE::Actor>(tid);
                if (!tf || tf->IsDead() || !tf->Is3DLoaded()) continue;

                // The engine's own combat-AI LoS read. a_arg2 is an out-param
                // the engine sets alongside the answer; only the return is the
                // verdict. Verified against the pinned NG rev: this thunks
                // RELOCATION_ID(53029, 53829) -- no VR id, but this function
                // is only ever reached through MainThread::Post, which is a
                // documented no-op on VR (pump refused), so VR never gets here.
                bool arg2 = false;
                const bool los = vf->HasLineOfSight(tf, arg2);

                std::lock_guard lk(g_mx);
                auto& e = g_cache[Key(a_viewer, tid)];
                // Transition-only logging (#22j): a stable verdict at pump
                // cadence would be a 7.5 Hz flood per follower-foe pair.
                const bool fresh = Since(e.at) <= kFreshSeconds;
                if (!fresh || e.los != los) {
                    spdlog::info("[los] {:08X} -> {:08X}: {}", a_viewer, tid,
                                 los ? "VISIBLE" : "OCCLUDED");
                }
                e.los = los;
                e.at  = Clock::now();
            }
        }

    }

    const char* VerdictName(Verdict a_v) {
        switch (a_v) {
        case Verdict::Visible:  return "visible";
        case Verdict::Occluded: return "occluded";
        default:                return "unknown";
        }
    }

    Verdict Check(RE::FormID a_viewer, RE::FormID a_target) {
        std::lock_guard lk(g_mx);
        const auto it = g_cache.find(Key(a_viewer, a_target));
        if (it == g_cache.end()) return Verdict::Unknown;
        if (Since(it->second.at) > kFreshSeconds) return Verdict::Unknown;
        return it->second.los ? Verdict::Visible : Verdict::Occluded;
    }

    void Want(RE::FormID a_viewer, std::vector<RE::FormID> a_targets) {
        if (!a_viewer || a_targets.empty()) return;
        {
            std::lock_guard lk(g_mx);
            auto& last = g_lastPost[a_viewer];
            if (Since(last) < kRepostSeconds) return;
            last = Clock::now();
        }
        // Post OUTSIDE our lock (leaf-mutex discipline; MainThread has its own
        // queue mutex). Capture by value: FormIDs, never handles or pointers,
        // so the frame that runs this re-resolves against the live world.
        MainThread::Post([a_viewer, targets = std::move(a_targets)]() {
            Measure(a_viewer, targets);
        });
    }

    void ClearTransientState() {
        std::lock_guard lk(g_mx);
        g_cache.clear();
        g_lastPost.clear();
    }

    namespace {

        // Distance from point p to segment [a,b] -- but only when p projects
        // BETWEEN the endpoints (an ally behind the caster or past the target
        // is not in the line of fire). Returns +inf otherwise. Same math as
        // CasterConsent's SegDist (its copy is private to the CheckCast hook).
        float SegDist(const RE::NiPoint3& p, const RE::NiPoint3& a, const RE::NiPoint3& b) {
            const RE::NiPoint3 ab = b - a, ap = p - a;
            const float len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
            if (len2 <= 1.0f) return ap.Length();
            const float t = (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2;
            if (t < 0.0f || t > 1.0f) return std::numeric_limits<float>::max();
            const RE::NiPoint3 proj{ a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t };
            return (p - proj).Length();
        }

        // ~a body off the line, CasterConsent's kLinePad.
        constexpr float kFireLinePad = 80.0f;

    }

    // NOTE on g_active: rebuilt by Followers::Refresh on the MAIN thread and
    // read here on the worker with no guard -- the same inherited, unguarded
    // pattern Scheduler::Tick, Actuation's NearestAlly and the [cast] sink's
    // IsTracked already use (there is no snapshot idiom to borrow). This adds
    // a reader to an existing pattern, not a new one.
    bool TeammateInFireLine(RE::FormID a_caster, RE::FormID a_target) {
        if (!a_caster || !a_target || a_caster == a_target) return false;
        auto* cf = RE::TESForm::LookupByID<RE::Actor>(a_caster);
        auto* tf = RE::TESForm::LookupByID<RE::Actor>(a_target);
        if (!cf || !tf) return false;   // fail open -- see the header
        const auto cpos = cf->GetPosition();
        const auto tpos = tf->GetPosition();

        // THE PLAYER FIRST. The CheckCast hook's WouldHitTeammate walks
        // highActorHandles, which never contains the player -- a beam swept
        // across the player was invisible to it. A stream is exactly the
        // spell shape that catches the player, so the player is checked here
        // by name (and skipped when the player IS the intended target: a
        // heal stream aimed at the player must not hold on the player).
        if (auto* pc = RE::PlayerCharacter::GetSingleton();
            pc && pc != tf && pc != cf &&
            SegDist(pc->GetPosition(), cpos, tpos) <= kFireLinePad) {
            return true;
        }
        for (const auto& h : Followers::g_active) {
            auto ptr = h.get();   // HOLD the NiPointer (Targeting rule)
            auto* ally = ptr.get();
            if (!ally || ally == cf || ally == tf) continue;
            if (ally->IsDead() || !ally->Is3DLoaded()) continue;
            if (SegDist(ally->GetPosition(), cpos, tpos) <= kFireLinePad) return true;
        }
        return false;
    }

}
