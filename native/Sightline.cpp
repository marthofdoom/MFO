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

        // MAIN THREAD ONLY. MFO's OWN physics raycast, fired ONLY when the
        // engine LoS already says CLEAR -- it exists to catch what
        // HasLineOfSight ignores (camp tents, cloth, some anim-static geometry)
        // and to forgive a foe one step up or down.
        //
        // Layer: kCharController. We ask "could a walking body travel this
        // line?" -- solid nav geometry (walls, tent walls, closed doors) stops
        // it, while a THIN point-ray at head/torso height passes OVER a railing
        // and THROUGH an open doorway/gate (there is no char-controller
        // collision in the opening), so we do not over-block on the things a
        // spell should sail past. The caster's own capsule is excluded by
        // matching the ray's system group to the caster's; the target's own
        // capsule is excluded by ending each ray a margin short of the sampled
        // body point (kCharController rays otherwise stop on any actor capsule).
        //
        // Multi-sample: feet / torso / head. If ANY sample is clear the verdict
        // is VISIBLE -- a foe one stair up, or behind a low sill, is not
        // "occluded." Returns true ONLY when every sample is blocked.
        //
        // FAIL-OPEN: no parent cell, no bhkWorld, or an unresolved controller
        // returns false (clear). We never manufacture an occlusion we cannot
        // prove, so the engine's own VISIBLE verdict stands.
        bool CustomRayConfirmsOcclusion(RE::Actor* a_vf, RE::Actor* a_tf) {
            auto* cell = a_vf->GetParentCell();
            if (!cell) return false;
            auto* world = cell->GetbhkWorld();
            if (!world) return false;

            // System group of the caster, so the ray skips the caster's own
            // char-controller capsule (same non-zero group => not collided).
            std::uint32_t casterFilter = 0;
            if (auto* cc = a_vf->GetCharController()) cc->GetCollisionFilterInfo(casterFilter);
            const std::uint32_t systemGroup = casterFilter >> 16;
            const std::uint32_t rayFilter =
                (systemGroup << 16) |
                static_cast<std::uint32_t>(RE::COL_LAYER::kCharController);

            // Eye origin: the true head/eye node (no camera offset -- this is a
            // world query, not a first-person aim).
            RE::NiPoint3 eye{}, dir{};
            a_vf->GetEyeVector(eye, dir, false);

            // Feet / torso / head of the target. GetHeight() is the live capsule
            // height; a nonsense value falls back to a nominal humanoid.
            const RE::NiPoint3 feet = a_tf->GetPosition();
            float h = a_tf->GetHeight();
            if (h <= 1.0f) h = 120.0f;
            const RE::NiPoint3 samples[3] = {
                { feet.x, feet.y, feet.z + 16.0f },        // just off the floor (dodge terrain)
                { feet.x, feet.y, feet.z + h * 0.55f },    // torso
                { feet.x, feet.y, feet.z + h * 0.90f },    // head
            };

            const float scale = RE::bhkWorld::GetWorldScale();
            constexpr float kTargetMargin = 48.0f;  // clears the target's own ~30u capsule

            RE::BSReadLockGuard lock(world->worldLock);
            for (const auto& pt : samples) {
                // Pull the endpoint a margin short of the body along the ray so
                // the target's OWN capsule is never the thing we call a wall.
                RE::NiPoint3 to = pt;
                const RE::NiPoint3 seg = to - eye;
                const float len = seg.Length();
                if (len > kTargetMargin) {
                    const float f = (len - kTargetMargin) / len;
                    to = { eye.x + seg.x * f, eye.y + seg.y * f, eye.z + seg.z * f };
                }

                RE::bhkPickData pick;
                pick.rayInput.from = eye * scale;   // NiPoint3 -> hkVector4 (implicit)
                pick.rayInput.to   = to  * scale;
                pick.rayInput.enableShapeCollectionFilter = false;
                pick.rayInput.filterInfo = rayFilter;

                world->PickObject(pick);
                if (!pick.rayOutput.HasHit()) return false;  // a clear sample -> visible, done
            }
            return true;  // every sample blocked -> an occluder the engine saw through
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
                bool los = vf->HasLineOfSight(tf, arg2);

                // Cheap engine check FIRST. The custom ray runs ONLY when the
                // engine says CLEAR, so the extra pick is spent on the
                // ambiguous cases and an already-occluded foe costs nothing
                // more. The ray can only ever turn a VISIBLE into OCCLUDED
                // (catch a tent/cloth the engine saw through) -- it never
                // overturns an OCCLUDED, and it fails OPEN (VISIBLE stands) when
                // it cannot run. Both discrete and concentration casts reach
                // this only through Want()'s per-viewer repost throttle
                // (kRepostSeconds), so the pick is bounded to one batch per
                // ~0.3 s per caster -- NOT the 133 ms pump tick.
                if (los && CustomRayConfirmsOcclusion(vf, tf)) los = false;

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

    // #10 SEV-1: this can be called off the pump domain (the mid-stream ffWatch
    // runs from Packages::Pump), so it must NOT walk the live g_active vector the
    // job worker reallocates in Refresh. It reads the immutable FormID snapshot
    // Refresh publishes atomically under g_mx (ActiveSnapshot) and re-resolves
    // each id -- lock-free and UAF-free from any thread.
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
        if (auto active = Followers::ActiveSnapshot()) {
            for (const RE::FormID id : *active) {
                auto* ally = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!ally || ally == cf || ally == tf) continue;
                if (ally->IsDead() || !ally->Is3DLoaded()) continue;
                if (SegDist(ally->GetPosition(), cpos, tpos) <= kFireLinePad) return true;
            }
        }
        return false;
    }

}
