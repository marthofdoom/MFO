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

        // Two Occluded readings further apart than this are not "agreeing": the run
        // restarts. Mirrors the callers' trust window (Actuation kHealLosTrustSec,
        // 3 s), which cannot be included here.
        constexpr float kOccRunTrustSeconds = 3.0f;

        // Per-PAIR floor between measurements. PickFoe runs once per RULE per
        // tick, so one follower with five foe selectors would otherwise queue
        // five identical measurement batches into the same frame.
        // PER PAIR, NOT PER VIEWER (field 2026-09-29). The floor used to be one
        // stamp per viewer, so whichever batch posted first in a window DROPPED
        // every other batch that viewer asked for in the same 0.3 s -- the foe
        // selectors, CastAuto and the heal pick (PickAlly) all want different
        // targets from the same viewer on the same lap. The heal pick's ally
        // batch lost that race for seconds at a time (Jesper -> Herd measured at
        // 14:30:04.2, then not again until the apply at 14:30:09.9), its verdict
        // aged past kFreshSeconds into Unknown, Unknown passes, and the heal rule
        // named an occluded ally that the main thread then refused. Keyed per
        // pair, a batch only skips the targets measured in the last 0.3 s, so no
        // caller can starve another and the cost per pair is unchanged.
        constexpr float kRepostSeconds = 0.3f;

        // One verdict slot. A pair has TWO (Entry::s), one per Basis, so an
        // engine-basis reader (melee, engage-on-sight) never sees the own-ray
        // verdict and a spell/bow reader never sees the engine's cached bool.
        struct Slot {
            bool              los = false;
            Clock::time_point at{};
            // CONSECUTIVE Occluded measurements (reset by any Visible one). The heal
            // recipient check asks for two agreeing readings before it calls a sight
            // loss, so one flicker of a borderline line does not drop the recipient
            // (field 2026-09-30, Jesper -> Herd). Written with `los` under g_mx.
            std::uint16_t     occRun = 0;
        };
        struct Entry { Slot s[2]; };   // indexed by Idx(Basis)

        constexpr int Idx(Basis a_b) { return a_b == Basis::Own ? 1 : 0; }

        // Written by the MAIN thread (the measurement), read by the WORKER
        // (the evaluator) -- cross-thread by design, so a real lock (#4).
        // This mutex is a LEAF: nothing is called while holding it, so it can
        // never participate in an inversion with MainThread's queue mutex or
        // the combat-group lock the caller may be inside.
        std::mutex g_mx;
        std::unordered_map<std::uint64_t, Entry> g_cache;
        std::unordered_map<std::uint64_t, Clock::time_point> g_lastPost[2];   // [Idx(basis)], Key(viewer, target)

        constexpr std::uint64_t Key(RE::FormID a_viewer, RE::FormID a_target) {
            return (static_cast<std::uint64_t>(a_viewer) << 32) | a_target;
        }

        float Since(Clock::time_point a_t) {
            if (a_t.time_since_epoch().count() == 0) return 1.0e9f;
            return std::chrono::duration<float>(Clock::now() - a_t).count();
        }

        // MAIN THREAD ONLY. MFO's OWN physics raycast. Basis::Engine: fired ONLY
        // when the engine LoS already says CLEAR, to catch what HasLineOfSight
        // ignores (camp tents, cloth, some anim-static geometry) and to forgive a
        // foe one step up or down. Basis::Own (spells and bows): it is the WHOLE
        // verdict -- the engine's HasLineOfSight for an NPC viewer is a cached
        // bool the engine refreshes on its own schedule (no raycast), so it can be
        // stale or false whatever the geometry (field 2026-09-30).
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
        // "occluded." Blocked ONLY when every sample is blocked.
        //
        // Unavailable (no parent cell, no bhkWorld; with a_strict also no char
        // controller) is NOT a verdict. Engine basis: the engine's own VISIBLE
        // stands (fail-open, as before). Own basis: the caller uses the engine's
        // answer for that one measurement and logs it (principle 7: never mask).
        // a_why names the reason. The worldLock scope is unchanged: read lock
        // around the picks only, g_mx never held here.
        enum class Ray { Clear, Blocked, Unavailable };
        Ray CustomRay(RE::Actor* a_vf, RE::Actor* a_tf, bool a_strict, const char*& a_why) {
            auto* cell = a_vf->GetParentCell();
            if (!cell) { a_why = "no cell"; return Ray::Unavailable; }
            auto* world = cell->GetbhkWorld();
            if (!world) { a_why = "no bhkWorld"; return Ray::Unavailable; }
            if (a_strict && !a_vf->GetCharController()) { a_why = "no controller"; return Ray::Unavailable; }

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
                // Point-blank (within the margin): the segment would end inside the
                // target's own capsule, and nothing can stand in that gap -> clear.
                if (len <= kTargetMargin) return Ray::Clear;
                {
                    const float f = (len - kTargetMargin) / len;
                    to = { eye.x + seg.x * f, eye.y + seg.y * f, eye.z + seg.z * f };
                }

                RE::bhkPickData pick;
                pick.rayInput.from = eye * scale;   // NiPoint3 -> hkVector4 (implicit)
                pick.rayInput.to   = to  * scale;
                pick.rayInput.enableShapeCollectionFilter = false;
                pick.rayInput.filterInfo = rayFilter;

                world->PickObject(pick);
                if (!pick.rayOutput.HasHit()) return Ray::Clear;  // a clear sample -> visible, done
            }
            return Ray::Blocked;  // every sample blocked
        }

        // MAIN THREAD ONLY -- the actual raycast. Resolves ids fresh (a handle
        // captured on the worker could be a different actor by the time the
        // frame runs it) and refuses anything without loaded 3D: a raycast
        // against an unloaded ref answers nothing and asks the havok world
        // about geometry that is not there.
        // a_changeOnly (MeasureNow's caller): log a verdict only when it CHANGES or the
        // pair was never measured -- not again merely because the entry went stale, which
        // a probe slower than kFreshSeconds (a big party's service period) would hit on
        // every measurement.
        //
        // a_basis picks the verdict (see Basis in the header), set by the CALLER, never
        // inferred here:
        //   Engine -- today's two-stage: engine HasLineOfSight first, the own ray only
        //             when the engine says CLEAR (it can only turn VISIBLE to OCCLUDED).
        //   Own    -- our ray ALONE decides (any clear sample VISIBLE, all blocked
        //             OCCLUDED); HasLineOfSight is not consulted unless the ray cannot
        //             run, in which case its answer is used for that one measurement and
        //             the failure is logged (rate-limited).
        // Cost: one Measure per (viewer, target, basis) pair per kRepostSeconds through
        // Want(), at most 3 picks each (worldLock read-held per ray, exactly as before).
        void Measure(RE::FormID a_viewer, const std::vector<RE::FormID>& a_targets, Basis a_basis,
                     bool a_changeOnly = false) {
            auto* vf = RE::TESForm::LookupByID<RE::Actor>(a_viewer);
            if (!vf || vf->IsDead() || !vf->Is3DLoaded()) return;

            for (const auto tid : a_targets) {
                auto* tf = RE::TESForm::LookupByID<RE::Actor>(tid);
                if (!tf || tf->IsDead() || !tf->Is3DLoaded()) continue;

                bool los = true;
                if (a_basis == Basis::Own) {
                    const char* why = nullptr;
                    const Ray r = CustomRay(vf, tf, /*a_strict=*/true, why);
                    if (r == Ray::Unavailable) {
                        // Principle 7: say so, then use the engine's cached answer for this
                        // one measurement. Main thread only, so a plain static is safe.
                        bool arg2 = false;
                        los = vf->HasLineOfSight(tf, arg2);
                        static std::unordered_map<std::uint64_t, Clock::time_point> s_unavailLog;
                        if (s_unavailLog.size() > 256) s_unavailLog.clear();
                        auto& last = s_unavailLog[Key(a_viewer, tid)];
                        if (Since(last) >= 10.0f) {
                            last = Clock::now();
                            spdlog::warn("[los] {:08X} -> {:08X}: own ray unavailable ({}) -- engine cache used ({})",
                                         a_viewer, tid, why, los ? "VISIBLE" : "OCCLUDED");
                        }
                    } else {
                        los = (r == Ray::Clear);
                    }
                } else {
                    // The engine's own combat-AI LoS read. a_arg2 is an out-param
                    // the engine sets alongside the answer; only the return is the
                    // verdict. Verified against the pinned NG rev: this thunks
                    // RELOCATION_ID(53029, 53829) -- no VR id, but this function
                    // is only ever reached through MainThread::Post, which is a
                    // documented no-op on VR (pump refused), so VR never gets here.
                    bool arg2 = false;
                    los = vf->HasLineOfSight(tf, arg2);
                    // Cheap engine check FIRST; the ray runs ONLY on a CLEAR, fails OPEN.
                    const char* why = nullptr;
                    if (los && CustomRay(vf, tf, /*a_strict=*/false, why) == Ray::Blocked) los = false;
                }

                std::lock_guard lk(g_mx);
                auto& e = g_cache[Key(a_viewer, tid)].s[Idx(a_basis)];
                // Transition-only logging (#22j): a stable verdict at pump
                // cadence would be a 7.5 Hz flood per follower-foe pair.
                const bool fresh = Since(e.at) <= kFreshSeconds;
                const bool never = e.at.time_since_epoch().count() == 0;
                if (a_changeOnly ? (never || e.los != los) : (!fresh || e.los != los)) {
                    spdlog::info("[los] {:08X} -> {:08X}: {}{}", a_viewer, tid,
                                 los ? "VISIBLE" : "OCCLUDED", a_basis == Basis::Own ? " (own ray)" : "");
                }
                // A stale run is not "agreeing": readings further apart than the trust window
                // restart the count (the new reading is run 1). Measured against the PREVIOUS
                // reading's stamp, so it must run before e.at is refreshed.
                if (!never && Since(e.at) > kOccRunTrustSeconds) e.occRun = 0;
                e.los = los;
                e.at  = Clock::now();
                e.occRun = los ? 0 : static_cast<std::uint16_t>(std::min<int>(e.occRun + 1, 0xFFFF));
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

    Verdict Check(RE::FormID a_viewer, RE::FormID a_target, Basis a_basis) {
        return CheckWithin(a_viewer, a_target, kFreshSeconds, a_basis);
    }

    Verdict CheckWithin(RE::FormID a_viewer, RE::FormID a_target, float a_maxAgeSeconds, Basis a_basis) {
        std::lock_guard lk(g_mx);
        const auto it = g_cache.find(Key(a_viewer, a_target));
        if (it == g_cache.end()) return Verdict::Unknown;
        const auto& e = it->second.s[Idx(a_basis)];
        if (Since(e.at) > a_maxAgeSeconds) return Verdict::Unknown;   // also the never-measured slot
        return e.los ? Verdict::Visible : Verdict::Occluded;
    }

    int OccludedRun(RE::FormID a_viewer, RE::FormID a_target, float a_maxAgeSeconds, Basis a_basis) {
        std::lock_guard lk(g_mx);
        const auto it = g_cache.find(Key(a_viewer, a_target));
        if (it == g_cache.end()) return 0;
        const auto& e = it->second.s[Idx(a_basis)];
        if (e.los) return 0;
        if (Since(e.at) > a_maxAgeSeconds) return 0;
        return e.occRun;
    }

    void Want(RE::FormID a_viewer, std::vector<RE::FormID> a_targets, Basis a_basis) {
        if (!a_viewer || a_targets.empty()) return;
        auto& lastPost = g_lastPost[Idx(a_basis)];   // throttled per (pair, basis)
        {
            std::lock_guard lk(g_mx);
            // Bounded: a long session meets many foes. Stamps older than the
            // floor carry no information, so drop them when the map grows.
            if (lastPost.size() > 1024)
                std::erase_if(lastPost, [](const auto& kv) { return Since(kv.second) >= kRepostSeconds; });
            const auto now = Clock::now();
            std::erase_if(a_targets, [&](RE::FormID t) {
                auto& last = lastPost[Key(a_viewer, t)];
                if (Since(last) < kRepostSeconds) return true;   // measured (or queued) just now
                last = now;
                return false;
            });
            if (a_targets.empty()) return;
        }
        // Post OUTSIDE our lock (leaf-mutex discipline; MainThread has its own
        // queue mutex). Capture by value: FormIDs, never handles or pointers,
        // so the frame that runs this re-resolves against the live world.
        MainThread::Post([a_viewer, a_basis, targets = std::move(a_targets)]() {
            Measure(a_viewer, targets, a_basis);
        });
    }

    Verdict MeasureNow(RE::FormID a_viewer, RE::FormID a_target, Basis a_basis) {
        if (!a_viewer || !a_target || REL::Module::IsVR()) return Verdict::Unknown;
        auto* vf = RE::TESForm::LookupByID<RE::Actor>(a_viewer);
        auto* tf = RE::TESForm::LookupByID<RE::Actor>(a_target);
        // Measure() skips these without writing, and an older cache entry for the pair
        // must not answer for a measurement that did not happen.
        if (!vf || vf->IsDead() || !vf->Is3DLoaded()) return Verdict::Unknown;
        if (!tf || tf->IsDead() || !tf->Is3DLoaded()) return Verdict::Unknown;
        Measure(a_viewer, { a_target }, a_basis, /*a_changeOnly=*/true);
        return Check(a_viewer, a_target, a_basis);
    }

    void ClearTransientState() {
        std::lock_guard lk(g_mx);
        g_cache.clear();
        g_lastPost[0].clear();
        g_lastPost[1].clear();
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
