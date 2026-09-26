// logistics/Lockpick.cpp -- FOLLOWER LOCKPICKING, chests (LP-M1, ClickUp 86e3edgha, batch L).
// Design: _research/lockpick-design-2026-09-24.md (sections 1-5, round LP-M1). A new mechanism,
// so its own file in the logistics subsystem (CLAUDE.md "a new mechanism is a new file").
//
// WHAT IT REPLACES. The loot scan used to admit a locked container when the follower's
// Lockpicking skill met a flat per-level threshold (the old LockPickable) and then looted
// THROUGH the lock (RemoveItem ignores locks): no pick, no animation, the lock stayed locked.
// Now:
//   * Admit()  (scan gate, worker): the JUDGE's gates -- ownership (#22e) and IsOffLimits, a
//     chest (not a door: LP-M2), a humanoid follower, a key or at least one lockpick, the lock
//     not requires-key without the key, Harbinger v17 + the verified Unlock seat present, and
//     no standing failed verdict. Refusals are logged once per (follower, lock, reason).
//   * Step()   (the excursion's ARRIVAL, worker): travel -> a main-thread snapshot (skill, the
//     follower's lockpick entry points through the narrow progression query, the GMSTs / INI
//     settings, picks, key) -> the seeded SIMULATION, run once WITHOUT a pick cap, giving the
//     breaks B the lock needs -> a KNOWN FAILURE IS NEVER STARTED (B >= picks held, or a sweet
//     spot <= 0): refused with zero consumption and a standing verdict that lifts when his
//     picks exceed B or his skill rises -> Harbinger ch.1 stand-still + ch.12 v2 IdleLockPick
//     at the lock for the simulated seconds, re-played on the clip-length cadence for the
//     whole window -> ONE main-thread post that re-validates, removes the B broken picks from
//     the FOLLOWER's inventory and calls the engine's own Unlock (bound by id, verified row)
//     -> the lock reads unlocked -> the existing transfer (StripCorpse) runs.
//   * LootHere refuses a locked ref (logistics/LootTake.cpp), so nothing loots through a lock.
//
// THE SIMULATION (LP-R0, disassembly of both runtimes; agentlog mfo-lockpick.md). The engine
// has no closed-form "odds": the LockpickingMenu is a dexterity minigame. Its quantities:
//   sweet  = fSweetSpot{lvl} * (fLockpickSkillSweetSpotBase + Mult*skill) * EP59
//            + numBrokenPicks * fLockpickBrokenPicksMult   (numBrokenPicks is 0 at Show)
//   partial= fPartialPick{lvl} * (fLockpickSkillPartialPickBase + Mult*skill)
//   life   = (fLockpickBreakSkillBase + fLockpickBreakSkillMult*skill) * fLockpickBreak{lvl}
//   center = uniform in [sweet/2 - fPickMaxAngle, fPickMaxAngle - sweet/2]
//   start  = 0, or with EP63 > 0 uniform in [max(center-EP63, lo), min(center+EP63, hi)]
//   allowed lock turn at pick angle p (d = |p - center|): d <= sweet/2 -> fLockMaxAngle (opens);
//            d <= sweet/2 + partial -> (1 - (d - sweet/2)/partial) * fLockMaxAngle; else 0
//   the lock turns at fLockRotationSpeed deg/s and springs back at twice that;
//   TENSION: while the lock is held at its allowed angle (< max) the pick's health drains at
//            100 / life per second (a pick survives `life` seconds of cumulative binding;
//            health carries across probes of the same pick); EP65 nonzero = no drain; life 0
//            = an instant break; a NEGATIVE life makes the drain positive and the engine
//            clamps health at 100, so it never breaks (treated so here, and logged); a break
//            costs one lockpick and resets health to 100;
//   success = the lock reaches fLockMaxAngle, then fUnlockDoorDelay seconds.
// The SEARCHER is the one modelled (non-engine) part. Phase A sweeps from the start angle with
// a stride of 2*(sweet/2 + partial) - sweet/2: the felt window around the center is
// sweet/2 + partial wide on each side, and that stride leaves every center within
// (partial + sweet/4) of some probe, so no center is missed. Phase B INFERS the distance from
// how far the lock turned, d = sweet/2 + partial * (1 - A / fLockMaxAngle) (the allowed-angle
// formula solved for d), and probes best +/- d (the side order is seeded); the right side
// lands on the center. The searcher holds a binding lock for kBindReactSec before letting go.
// Seeded per (follower, lock) so a re-issue of the same lock replays the same outcome.
//
// THREADING. Admit / Step / SweepStale / Abort / Clear run on the tick's job worker (#4, #72);
// their state is worker-only. The engine reads the snapshot needs (perk entry points evaluate
// conditions; the skill; settings) run on the TRUE main thread through MainThread::Post and
// come back through g_snapMx. The world writes (RemoveItem of picks, the Unlock) run in one
// MainThread::Post. VR (no main-thread pump) = inert.
//
// DEGRADE. Harbinger absent or below ABI v17, a synchronous v2-idle refusal, or the Unlock
// row refused by the self-check = lockpicking is INERT (locked chests stay skipped, dLocked),
// logged once. There is no direct road: a pick without an animation is not allowed.
#include "Logistics_internal.h"
#include "apmf/APMFBridge.h"   // ch.1 + ch.12 v2 lockpick hold (APMF ABI v17)
#include "Runtime.h"           // SeatVerified(): the mit-3.7 F1 self-check gate (Lockpick.Unlock row)
#include "Scheduler.h"         // ServiceClock(): the UNPAUSED service clock every pick timer runs on
#include "LocationTypes.h"     // LP-M2: the load-door destination bar shares the town/inn table

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace MFO::Logistics::Lockpick {

    namespace {
        // Skyrim.esm forms. The Lockpick MISC is the one fixed record the loot code already
        // keys on (LootTake.cpp LootLockpicks, 0x0000000A); IdleLockPick is the vanilla IDLE
        // (anim event IdleLockPick, mt_lockpick.hkx, no conditions; Harbinger INTEGRATION.md
        // "Playing an idle at a target": about 5.6 s, one-shot).
        constexpr RE::FormID kLockpickForm = 0x0000000A;
        constexpr RE::FormID kIdleLockPick = 0x000BB051;

        // THE ONE MODELLED CONSTANT (not an engine number): how long the simulated searcher
        // holds a binding lock before releasing it. The engine gives the drain RATE (100/life
        // per second, LP-R0); how long a hand keeps pressure on a lock that has stopped
        // turning is the player's reaction, which no binary contains. A quarter second is a
        // typical human visual reaction. Flagged for review.
        constexpr float kBindReactSec = 0.25f;
        // A backstop on the number of probes. With a sweet spot > 0 (a sweet spot <= 0 is
        // refused before simulating) the searcher succeeds in a handful of probes; this only
        // bounds float corner cases.
        constexpr int   kProbeCap  = 512;

        // THE IdleLockPick CLIP LENGTH (seconds). The clip is one-shot, so it is re-played on
        // this cadence from the last play for the WHOLE pick window (marth: proper animations
        // for ALL actions). 5.6 s is Harbinger INTEGRATION.md's figure for 0x0BB051; CORRECT
        // IT from the first field log's Release line ("the graph raised IdleStop N ms after
        // the call"). Never re-played on a pick break mid-clip.
        constexpr double kIdleClipSec = 5.6;

        // All three floors below run on the Scheduler's UNPAUSED service clock (seconds), so
        // a menu or a pause never ages a pick. FLOORS, not expiries (principle 9).
        // A snapshot that never comes back (the main-thread pump stalled) ends the attempt
        // loudly after this long; the pump drains every frame.
        constexpr double kSnapshotFloorSec = 5.0;
        // A posted Unlock that has not shown up on the ref after this long is a failure,
        // logged, never retried in place (one frame is the real latency).
        constexpr double kUnlockFloorSec = 3.0;
        // A pick job whose arrival step has not run for this long is abandoned (the caller
        // stopped driving it: a courtesy hold, a leash, a teleport). Three logistics ticks
        // (kLogisticsInterval is 1 s), so a live job is never aged out. Abandoning releases
        // the hold and consumes nothing.
        constexpr double kStaleFloorSec = 3.0;

        enum class Reason : std::uint8_t {
            kNone, kOwned, kOffLimits, kNotChest, kRequiresKey, kNoPicks, kCreature,
            kNoHarbinger, kNoMainThread, kUnlockSeat, kSimFail, kTooLong, kNotLocked,
            kGone, kSettings, kIdleEnded, kUnlockFailed, kNoTime, kNoSweetSpot, kDoorInto,
        };
        const char* ReasonName(Reason a_r) {
            switch (a_r) {
            case Reason::kNone:         return "none";
            case Reason::kOwned:        return "owned";
            case Reason::kOffLimits:    return "offlimits";
            case Reason::kNotChest:     return "notChestOrDoor";
            case Reason::kRequiresKey:  return "requiresKey";
            case Reason::kNoPicks:      return "noPicks";
            case Reason::kCreature:     return "creature";
            case Reason::kNoHarbinger:  return "noHarbinger";
            case Reason::kNoMainThread: return "noMainThread";
            case Reason::kUnlockSeat:   return "unlockSeatRefused";
            case Reason::kSimFail:      return "simFail";
            case Reason::kTooLong:      return "tooLong";
            case Reason::kNotLocked:    return "notLocked";
            case Reason::kGone:         return "gone";
            case Reason::kSettings:     return "settingMissing";
            case Reason::kIdleEnded:    return "idleEnded";
            case Reason::kUnlockFailed: return "unlockFailed";
            case Reason::kNoTime:       return "noTime";
            case Reason::kNoSweetSpot:  return "noSweetSpot";
            case Reason::kDoorInto:     return "doorDestinationBarred";
            }
            return "?";
        }
        const char* LevelName(int a_lvl) {
            static constexpr const char* kNames[] = { "Novice", "Apprentice", "Adept", "Expert", "Master" };
            return (a_lvl >= 0 && a_lvl < 5) ? kNames[a_lvl] : (a_lvl == 5 ? "RequiresKey" : "?");
        }

        // ── the engine's Unlock, bound by id (not in CommonLib 3.7) ─────────────────
        // 1.6.1170 id 20226 @0x2FAF00 / 1.5.97 id 19821 @0x2A75B0, void(TESObjectREFR*):
        // ExtraLock -> REFR_LOCK::SetLocked(false) (12401 / 12274, clears numTries) -> tail-jumps
        // 19512 / 19110 (AddChange(0x1000 kLockExtra) + the lock-changed script event). The
        // LockpickingMenu's own success path calls it (51968 / 51088 +0xA8). Row
        // "Lockpick.Unlock" in native/VerifiedAddresses.h. MAIN THREAD ONLY.
        using UnlockFn = void(RE::TESObjectREFR*);
        REL::Relocation<UnlockFn>& UnlockRel() {
            static REL::Relocation<UnlockFn> func{ REL::RelocationID(19821, 20226) };
            return func;
        }
        // Worker-safe (a static, the self-check result is a thread-safe static). VR has no
        // sourced id: refused rather than guessed.
        bool UnlockSeatOk() {
            static const bool ok = [] {
                if (REL::Module::IsVR()) {
                    spdlog::warn("[lockpick] VR: the engine Unlock has no verified id -- lockpicking is inert");
                    return false;
                }
                return Runtime::SeatVerified(UnlockRel().address(), "Lockpick.Unlock");
            }();
            return ok;
        }
        void UnlockOnMain(RE::TESObjectREFR* a_ref) {
            if (!a_ref || !UnlockSeatOk()) return;
            UnlockRel()(a_ref);
        }

        std::int32_t CountOf(RE::TESObjectREFR* a_holder, RE::TESBoundObject* a_obj) {
            if (!a_holder || !a_obj) return 0;
            auto counts = a_holder->GetInventoryCounts([a_obj](RE::TESBoundObject& o) { return &o == a_obj; });
            const auto it = counts.find(a_obj);
            return it == counts.end() ? 0 : it->second;
        }
        RE::TESBoundObject* LockpickObject() {
            static RE::TESBoundObject* const pick = RE::TESForm::LookupByID<RE::TESBoundObject>(kLockpickForm);
            return pick;
        }

        // ── the settings the minigame reads (all live: ESM / overhaul overrides apply) ──
        struct Settings {
            float sweet[5]{}, sweetBase = 0, sweetMult = 0, brokenMult = 0;
            float partial[5]{}, partialBase = 0, partialMult = 0;
            float brk[5]{}, brkBase = 0, brkMult = 0;
            float pickMax = 0, lockMax = 0, rotSpeed = 0, unlockDelay = 0;
        };
        // MAIN THREAD. False (with the first missing name) if any setting is absent: the
        // attempt is refused loudly, never run on a guessed constant.
        bool ReadSettings(Settings& a_s, std::string& a_missing) {
            auto* gsc = RE::GameSettingCollection::GetSingleton();
            auto* ini = RE::INISettingCollection::GetSingleton();
            auto gmst = [&](const char* a_name, float& a_out) {
                auto* st = gsc ? gsc->GetSetting(a_name) : nullptr;
                if (!st) { if (a_missing.empty()) a_missing = a_name; return; }
                a_out = st->GetFloat();
            };
            auto inif = [&](const char* a_name, float& a_out) {
                auto* st = ini ? ini->GetSetting(a_name) : nullptr;
                if (!st) { if (a_missing.empty()) a_missing = a_name; return; }
                a_out = st->GetFloat();
            };
            static constexpr const char* kSweet[5]   = { "fSweetSpotVeryEasy", "fSweetSpotEasy", "fSweetSpotAverage",
                                                         "fSweetSpotHard", "fSweetSpotVeryHard" };
            static constexpr const char* kPartial[5] = { "fPartialPickVeryEasy", "fPartialPickEasy", "fPartialPickAverage",
                                                         "fPartialPickHard", "fPartialPickVeryHard" };
            static constexpr const char* kBreak[5]   = { "fLockpickBreakNovice", "fLockpickBreakApprentice",
                                                         "fLockpickBreakAdept", "fLockpickBreakExpert",
                                                         "fLockpickBreakMaster" };
            for (int i = 0; i < 5; ++i) {
                gmst(kSweet[i], a_s.sweet[i]);
                gmst(kPartial[i], a_s.partial[i]);
                gmst(kBreak[i], a_s.brk[i]);
            }
            gmst("fLockpickSkillSweetSpotBase", a_s.sweetBase);
            gmst("fLockpickSkillSweetSpotMult", a_s.sweetMult);
            gmst("fLockpickBrokenPicksMult", a_s.brokenMult);
            gmst("fLockpickSkillPartialPickBase", a_s.partialBase);
            gmst("fLockpickSkillPartialPickMult", a_s.partialMult);
            gmst("fLockpickBreakSkillBase", a_s.brkBase);
            gmst("fLockpickBreakSkillMult", a_s.brkMult);
            gmst("fPickMaxAngle", a_s.pickMax);
            inif("fLockMaxAngle:Interface", a_s.lockMax);
            inif("fLockRotationSpeed:Interface", a_s.rotSpeed);
            inif("fUnlockDoorDelay:Interface", a_s.unlockDelay);
            if (a_missing.empty() && (a_s.lockMax <= 0.0f || a_s.rotSpeed <= 0.0f)) a_missing = "fLockMaxAngle/fLockRotationSpeed <= 0";
            return a_missing.empty();
        }

        // ── THE SIMULATION (pure; see the file banner) ─────────────────────────────
        struct SplitMix {
            std::uint64_t s;
            std::uint64_t Next() {
                std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                return z ^ (z >> 31);
            }
            // [0,1) from 32 bits, the engine's own scale (rand32 * 2^-32).
            float U() { return static_cast<float>(static_cast<std::uint32_t>(Next() >> 32)) * 2.3283064e-10f; }
        };

        struct SimIn {
            float    skill = 0;
            int      level = 0;        // 0..4
            float    ep59 = 1.0f, ep63 = 0.0f;
            bool     unbreakable = false;
            Settings s{};
            std::uint64_t seed = 0;
        };
        // The run is UNCAPPED on picks: `broken` is the number of breaks the lock needs (B).
        // With P picks held the seeded attempt succeeds exactly when B < P (the path up to the
        // success probe does not depend on how many picks are held).
        struct SimOut {
            bool   success = false;    // false only when no sweet spot could be found
            int    broken = 0;         // B: breaks before the opening probe
            float  seconds = 0.0f;
            int    probes = 0;
            bool   probeCap = false;
            bool   negativeLife = false;
            float  sweet = 0, partial = 0, life = 0, center = 0, start = 0;
        };

        SimOut Simulate(const SimIn& a_in) {
            SimOut o{};
            const Settings& s = a_in.s;
            const int L = std::clamp(a_in.level, 0, 4);
            SplitMix rng{ a_in.seed };
            // Show (51964 / 51085): numBrokenPicks is 0 at Show, so the broken-picks term is 0.
            o.sweet   = s.sweet[L] * (s.sweetBase + s.sweetMult * a_in.skill) * a_in.ep59 + 0.0f * s.brokenMult;
            o.partial = s.partial[L] * (s.partialBase + s.partialMult * a_in.skill);
            o.life    = (s.brkBase + s.brkMult * a_in.skill) * s.brk[L] * 1.0f;
            if (o.sweet <= 0.0f) {       // never opens: the caller refuses before starting
                o.probeCap = true;
                return o;
            }
            const float half = o.sweet * 0.5f;
            const float partial = std::max(o.partial, 0.0f);
            const float lo = half - s.pickMax, hi = s.pickMax - half;
            o.center = lo + rng.U() * (hi - lo);
            float p0 = 0.0f;
            if (a_in.ep63 > 0.0f) {
                const float a = std::max(o.center - a_in.ep63, lo);
                const float b = std::min(o.center + a_in.ep63, hi);
                p0 = a + rng.U() * (b - a);
            }
            o.start = p0;
            // The engine: health += (-100 / life) * dt, clamped to [0, 100]. life < 0 -> the
            // drain is positive and health stays at 100: no break. life == 0 -> an instant break.
            o.negativeLife = o.life < 0.0f;
            const bool noWear = a_in.unbreakable || o.negativeLife;

            auto allowed = [&](float a_p) {
                const float d = std::fabs(a_p - o.center);
                if (d <= half) return s.lockMax;
                if (partial <= 0.0f || d > half + partial) return 0.0f;
                return (1.0f - (d - half) / partial) * s.lockMax;
            };
            float t = 0.0f, health = 100.0f;
            bool done = false;
            // One probe: turn, and either open or bind (and maybe break), then let go.
            auto probe = [&](float a_p) {
                ++o.probes;
                const float A = allowed(a_p);
                if (A >= s.lockMax) {
                    t += s.lockMax / s.rotSpeed + s.unlockDelay;
                    o.success = true;
                    done = true;
                    return A;
                }
                t += A / s.rotSpeed;              // the lock turns until it binds
                float bind = kBindReactSec;
                if (!noWear) {
                    if (o.life == 0.0f) {         // the engine's zero-life branch: an instant break
                        bind = 0.0f;
                        health = 0.0f;
                    } else {
                        const float drain = 100.0f / o.life;
                        const float left  = health / drain;
                        if (left <= bind) { bind = left; health = 0.0f; }
                        else              health -= bind * drain;
                    }
                }
                t += bind;
                if (!noWear && health <= 0.0f) {
                    ++o.broken;
                    health = 100.0f;
                }
                t += A / (2.0f * s.rotSpeed);     // spring back at twice the turn speed
                return A;
            };
            auto inDomain = [&](float a_p) { return a_p >= -s.pickMax && a_p <= s.pickMax; };

            // Phase A: sweep outward from the start with stride half + 2*partial (see the
            // banner: every center is within partial + sweet/4 < sweet/2 + partial of a probe).
            const float stride = half + 2.0f * partial;
            const float dir0 = (rng.Next() & 1ull) ? 1.0f : -1.0f;
            // A step that would leave the pick's range probes the range END once instead (the
            // last in-range probe and the end are less than a stride apart, so coverage holds
            // up to the edge), then that side is exhausted.
            float best = 0.0f, bestA = -1.0f;
            bool exhausted[2] = { false, false };
            for (int k = 0; !done && o.probes < kProbeCap; ++k) {
                bool any = false;
                for (int side = 0; side < (k == 0 ? 1 : 2) && !done; ++side) {
                    if (exhausted[side]) continue;
                    float p = p0 + static_cast<float>(k) * stride * (side == 0 ? dir0 : -dir0);
                    if (!inDomain(p)) {
                        p = std::clamp(p, -s.pickMax, s.pickMax);
                        exhausted[side] = true;
                    }
                    any = true;
                    const float A = probe(p);
                    if (A > 0.0f && A > bestA) { best = p; bestA = A; }
                }
                if (bestA > 0.0f || !any) break;
            }
            // Phase B: infer the distance from how far the lock turned and probe both sides.
            float dir = dir0;
            while (!done && bestA > 0.0f && o.probes < kProbeCap) {
                const float d = half + partial * (1.0f - bestA / s.lockMax);
                float nb = best, nA = bestA;
                for (int side = 0; side < 2 && !done; ++side) {
                    const float p = best + (side == 0 ? dir : -dir) * d;
                    if (!inDomain(p)) continue;
                    const float A = probe(p);
                    if (A > nA) { nb = p; nA = A; }
                }
                if (done) break;
                if (nA <= bestA) break;   // no progress (float corner): give up, logged by the caller
                if (nb < best) dir = -1.0f; else dir = 1.0f;
                best = nb; bestA = nA;
            }
            if (!done) o.probeCap = true;
            o.seconds = t;
            return o;
        }

        // ── per-follower state (worker-only) ──────────────────────────────────────
        std::uint64_t PairKey(RE::FormID a_f, RE::FormID a_r) {
            return (static_cast<std::uint64_t>(a_f) << 32) | a_r;
        }
        // A standing failed verdict: the lock is refused for this follower until his
        // lockpick count or his skill rises above what the verdict was reached with.
        // `picks` is the threshold: refused while he holds <= picks lockpicks (and his skill has
        // not risen). For simFail it is B, the breaks the lock needs, so it lifts at B+1 picks.
        struct FailVerdict { Reason why = Reason::kNone; std::int32_t picks = 0; float skill = 0.0f; };
        constexpr std::int32_t kNoPickCountHelps = 0x7FFFFFFF;   // retried on a skill rise only
        std::unordered_map<std::uint64_t, FailVerdict> g_fail;
        // Log-once memory: the last reason logged per (follower, lock).
        std::unordered_map<std::uint64_t, Reason> g_logged;
        bool g_loggedNoHarbinger = false;
        bool g_loggedNoMain = false;

        // The main-thread snapshot mailbox (main writes, worker reads).
        struct Snap {
            bool done = false;
            Reason refuse = Reason::kNone;
            std::string missing;           // kSettings: the first absent setting
            int   level = -1;
            float skill = 0.0f;
            std::int32_t picks = 0;
            bool  hasKey = false;
            Progression::LockpickEntryPoints ep{};
            Settings s{};
        };
        std::mutex g_snapMx;
        std::unordered_map<std::uint64_t, Snap> g_snaps;   // by ticket; guarded by g_snapMx
        std::uint64_t g_nextTicket = 1;                    // worker-only

        enum class Phase : std::uint8_t { kSnapshot, kPicking, kUnlocking };
        // Every time below is on the Scheduler's UNPAUSED service clock (Scheduler::ServiceClock).
        struct Job {
            RE::FormID    ref = 0;
            Phase         phase = Phase::kSnapshot;
            std::uint64_t ticket = 0;
            double        startedAt = 0.0;     // the attempt began (snapshot posted)
            double        lastStep = 0.0;      // the arrival step last ran (SweepStale reads it)
            double        filedAt = 0.0;       // the hold was filed (the never-live floor)
            double        liveAt = -1.0;       // Harbinger first read the idle claim live (-1 = not yet)
            double        lastPlay = 0.0;      // the last (re)play, relative to liveAt
            double        unlockPostedAt = 0.0;
            bool          viaKey = false;
            bool          holdFiled = false;
            int           replays = 0;
            float         window = 0.0f;       // the simulated seconds
            int           level = 0;
            float         skill = 0.0f;
            std::int32_t  picksHeld = 0;
            SimOut        sim{};
        };
        std::unordered_map<RE::FormID, Job> g_jobs;
        // LP-M2 F-L3: the last lock THIS follower's own job opened (picked or keyed), read and
        // erased once by the door leg's arrival (ConsumeOpenedByUs). Worker-only.
        std::unordered_map<RE::FormID, RE::FormID> g_openedByUs;

        void LogRefusal(RE::FormID a_f, RE::TESObjectREFR* a_ref, Reason a_why, const std::string& a_detail) {
            auto& last = g_logged[PairKey(a_f, a_ref->GetFormID())];
            if (last == a_why) return;
            last = a_why;
            const char* nm = a_ref->GetDisplayFullName();
            spdlog::info("[lockpick] {:08X}: {:08X} '{}' REFUSED ({}){}", a_f, a_ref->GetFormID(),
                         nm ? nm : "?", ReasonName(a_why), a_detail);
        }

        void EndJob(RE::FormID a_f, const char* a_why) {
            const auto it = g_jobs.find(a_f);
            if (it == g_jobs.end()) return;
            if (it->second.holdFiled) APMFBridge::ReleaseLockpickHold(a_f);
            if (a_why)
                spdlog::info("[lockpick] {:08X}: pick of {:08X} abandoned ({}) -- hold released, nothing "
                             "consumed, the lock stays locked", a_f, it->second.ref, a_why);
            {
                std::scoped_lock lk(g_snapMx);
                g_snaps.erase(it->second.ticket);
            }
            g_jobs.erase(it);
        }

        // ── LP-M2 DOORS: the design's section 3 rule 2 ────────────────────────────
        // A LOAD door (one with ExtraTeleport) is picked only when opening it is not trespass
        // by intent: its DESTINATION (the linked door on the far side) must not be owned (the
        // linked ref or its cell), not a player home / player-owned cell (RefInPlayerStorage,
        // the loot scan's own test), not crime-to-activate, and its location must be KNOWN and a
        // FIGHT SITE: LocationTypes::Classify (the SAME table and innermost-first rule as the
        // engage-on-sight town/inn filter) must read Clearable / Dungeon innermost. A lived-in
        // place, a destination with no location, or a location chain with no location type at
        // all is refused (fail closed: an untagged destination could be anyone's house).
        // Returns "" when the door may be picked, else the reason text. Worker, read-only.
        std::string LoadDoorBar(RE::TESObjectREFR* a_door) {
            auto* tele = a_door ? a_door->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
            if (!tele || !tele->teleportData) return {};   // not a load door: no destination to bar
            auto  lptr   = tele->teleportData->linkedDoor.get();
            auto* linked = lptr.get();
            if (!linked) return "a load door whose destination cannot be resolved";
            if (auto* o = linked->GetOwner()) return std::format("its destination door is owned ({:08X})", o->GetFormID());
            if (auto* cell = linked->GetParentCell(); cell && cell->GetOwner())
                return std::format("its destination cell is owned ({:08X})", cell->GetOwner()->GetFormID());
            if (RefInPlayerStorage(linked)) return "its destination is a player home";
            if (linked->IsOffLimits()) return "entering its destination is a crime";
            const RE::BGSLocation* loc = linked->GetCurrentLocation();
            if (!loc) return "its destination has no location (fail closed)";
            switch (LocationTypes::Classify(loc)) {
            case LocationTypes::Kind::kFightSite: return {};
            case LocationTypes::Kind::kCivilised: return "its destination is a lived-in place";
            case LocationTypes::Kind::kUntagged:  break;
            }
            return "its destination location has no location type (fail closed)";
        }

        // Session-wide inertness (Harbinger / VR / the Unlock seat). Logged once each.
        Reason InertReason() {
            if (!MainThread::IsInstalled()) {
                if (!g_loggedNoMain) {
                    g_loggedNoMain = true;
                    spdlog::warn("[lockpick] no main-thread pump (VR) -- follower lockpicking is inert");
                }
                return Reason::kNoMainThread;
            }
            if (!APMFBridge::LockpickIdleOffered()) {
                if (!g_loggedNoHarbinger) {
                    g_loggedNoHarbinger = true;
                    spdlog::info("[lockpick] Harbinger (APMF) absent, older than ABI v17, or its ch.12 idle v2 "
                                 "refused -- follower lockpicking is INERT (locked chests stay skipped). There "
                                 "is no direct road: a pick needs its animation.");
                }
                return Reason::kNoHarbinger;
            }
            if (!UnlockSeatOk()) return Reason::kUnlockSeat;
            return Reason::kNone;
        }
    }

    // ── Admit: the scan's lock gate (worker; read-only; inside the cell walk) ──────
    bool Admit(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, Clock::time_point /*a_now*/) {
        if (!a_follower || !a_ref) return false;
        const RE::FormID fid = a_follower->GetFormID();
        if (const Reason inert = InertReason(); inert != Reason::kNone) return false;
        // #22e: the scan bars owned / off-limits refs before this gate; re-checked here so
        // the judge's verdict never depends on the call order (and logs its reason).
        if (a_ref->GetOwner()) { LogRefusal(fid, a_ref, Reason::kOwned, ""); return false; }
        if (a_ref->IsOffLimits()) { LogRefusal(fid, a_ref, Reason::kOffLimits, ""); return false; }
        auto* base = a_ref->GetBaseObject();
        const bool door = base && base->Is(RE::FormType::Door);
        if (!base || (!door && !base->Is(RE::FormType::Container))) {
            LogRefusal(fid, a_ref, Reason::kNotChest, "");
            return false;
        }
        if (door) {   // LP-M2: a load door's destination bar (the door ref's own owner is barred above)
            if (const std::string bar = LoadDoorBar(a_ref); !bar.empty()) {
                LogRefusal(fid, a_ref, Reason::kDoorInto, " -- " + bar);
                return false;
            }
        }
        auto* race = a_follower->GetRace();
        if (!race || !race->HasKeywordString("ActorTypeNPC")) {
            LogRefusal(fid, a_ref, Reason::kCreature, " -- IdleLockPick has no clip outside the humanoid graph");
            return false;
        }
        auto* lock = a_ref->GetLock();
        if (!lock || !lock->IsLocked()) return true;   // nothing to pick (the caller asked on a stale read)
        const auto lvl = a_ref->GetLockLevel();
        const bool hasKey = lock->key && CountOf(a_follower, lock->key) > 0;
        const std::int32_t picks = CountOf(a_follower, LockpickObject());
        if (!hasKey) {
            if (lvl == RE::LOCK_LEVEL::kRequiresKey || lvl == RE::LOCK_LEVEL::kUnlocked) {
                LogRefusal(fid, a_ref, Reason::kRequiresKey, "");
                return false;
            }
            if (picks <= 0) { LogRefusal(fid, a_ref, Reason::kNoPicks, ""); return false; }
        }
        if (const auto it = g_fail.find(PairKey(fid, a_ref->GetFormID())); it != g_fail.end()) {
            auto* avo = a_follower->AsActorValueOwner();
            const float skill = avo ? avo->GetClampedActorValue(RE::ActorValue::kLockpicking) : 0.0f;
            if (picks <= it->second.picks && skill <= it->second.skill) {
                LogRefusal(fid, a_ref, it->second.why,
                           std::format(" -- standing verdict at {} pick(s), skill {:.0f}; retried when either rises",
                                       it->second.picks, it->second.skill));
                return false;
            }
            g_fail.erase(it);
        }
        g_logged.erase(PairKey(fid, a_ref->GetFormID()));   // admitted: a later refusal logs again
        return true;
    }

    // ── Step: the excursion's arrival at a LOCKED chest (worker) ──────────────────
    StepResult Step(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, Clock::time_point a_now,
                    Clock::time_point a_excursionEnd) {
        if (!a_follower || !a_ref) return StepResult::kRefused;
        const RE::FormID fid = a_follower->GetFormID();
        const RE::FormID rid = a_ref->GetFormID();

        auto jt = g_jobs.find(fid);
        if (jt != g_jobs.end() && jt->second.ref != rid) {
            EndJob(fid, "a different lock");
            jt = g_jobs.end();
        }
        if (jt == g_jobs.end()) {
            // A fresh attempt: the scan's gates again (state may have moved since the scan).
            if (!Admit(a_follower, a_ref, a_now)) return StepResult::kRefused;
            Job j{};
            j.ref       = rid;
            j.phase     = Phase::kSnapshot;
            j.ticket    = g_nextTicket++;
            j.startedAt = Scheduler::ServiceClock();
            j.lastStep  = j.startedAt;
            const std::uint64_t ticket = j.ticket;
            {
                std::scoped_lock lk(g_snapMx);
                g_snaps[ticket] = Snap{};
            }
            g_jobs.emplace(fid, j);
            // MAIN THREAD: everything that evaluates conditions or reads live settings.
            MainThread::Post([fid, rid, ticket]() {
                Snap s{};
                s.done = true;
                auto* f = RE::TESForm::LookupByID<RE::Actor>(fid);
                auto* r = RE::TESForm::LookupByID<RE::TESObjectREFR>(rid);
                auto* lock = r ? r->GetLock() : nullptr;
                if (!f || !r || f->IsDead()) {
                    s.refuse = Reason::kGone;
                } else if (!lock || !lock->IsLocked()) {
                    s.refuse = Reason::kNotLocked;
                } else {
                    s.level  = static_cast<int>(r->GetLockLevel());
                    s.hasKey = lock->key && CountOf(f, lock->key) > 0;
                    s.picks  = CountOf(f, LockpickObject());
                    auto* avo = f->AsActorValueOwner();
                    s.skill  = avo ? avo->GetClampedActorValue(RE::ActorValue::kLockpicking) : 0.0f;
                    s.ep     = Progression::LockpickEntryPointsFor(f, r);
                    if (!ReadSettings(s.s, s.missing)) s.refuse = Reason::kSettings;
                }
                std::scoped_lock lk(g_snapMx);
                if (auto it = g_snaps.find(ticket); it != g_snaps.end()) it->second = std::move(s);
                // else: the job was abandoned before this ran -- drop the result
            });
            return StepResult::kHold;
        }

        Job& j = jt->second;
        // The UNPAUSED service clock: the caller runs only on unpaused services, and the
        // clock itself advances only while the game runs (Scheduler.cpp g_serviceClock).
        const double clk = Scheduler::ServiceClock();
        j.lastStep = clk;

        if (j.phase == Phase::kSnapshot) {
            Snap s{};
            {
                std::scoped_lock lk(g_snapMx);
                const auto it = g_snaps.find(j.ticket);
                if (it != g_snaps.end() && it->second.done) {
                    s = it->second;
                    g_snaps.erase(it);
                }
            }
            if (!s.done) {
                if (clk - j.startedAt < kSnapshotFloorSec) return StepResult::kHold;
                spdlog::error("[lockpick] {:08X}: the main-thread snapshot for {:08X} never came back ({:.0f}s of "
                              "running game) -- attempt dropped", fid, rid, kSnapshotFloorSec);
                EndJob(fid, nullptr);
                return StepResult::kRefused;
            }
            if (s.refuse == Reason::kNotLocked) { EndJob(fid, nullptr); return StepResult::kProceed; }
            if (s.refuse != Reason::kNone) {
                LogRefusal(fid, a_ref, s.refuse, s.missing.empty() ? "" : std::format(" -- '{}' not found", s.missing));
                EndJob(fid, nullptr);
                return StepResult::kRefused;
            }
            j.level     = s.level;
            j.skill     = s.skill;
            j.picksHeld = s.picks;
            j.viaKey    = s.hasKey;
            const auto pairKey = PairKey(fid, rid);
            if (j.viaKey) {
                // The player's key road: no pick, no odds. The follower still plays the
                // idle at the lock for the success tail (the lock turning home + the delay).
                j.sim = SimOut{};
                j.sim.success = true;
                j.sim.seconds = s.s.lockMax / s.s.rotSpeed + s.s.unlockDelay;
            } else {
                if (s.level < 0 || s.level > 4 || s.picks <= 0) {
                    LogRefusal(fid, a_ref, s.picks <= 0 ? Reason::kNoPicks : Reason::kRequiresKey, "");
                    EndJob(fid, nullptr);
                    return StepResult::kRefused;
                }
                SimIn in{};
                in.skill       = s.skill;
                in.level       = s.level;
                in.ep59        = s.ep.sweetSpotMult;
                in.ep63        = s.ep.startingArc;
                in.unbreakable = s.ep.unbreakable;
                in.s           = s.s;
                in.seed        = pairKey ^ 0x4C4F434B5049434Bull;   // "LOCKPICK"
                j.sim = Simulate(in);   // UNCAPPED on picks: j.sim.broken = B
            }
            j.window = j.sim.seconds;
            const float left = std::chrono::duration<float>(a_excursionEnd - a_now).count();
            const bool enough = j.viaKey || (j.sim.success && j.sim.broken < j.picksHeld);
            spdlog::info("[lockpick] {:08X}: {:08X} '{}' {} lock ({}), skill {:.0f}, picks {}{} | sweet {:.2f} "
                         "partial {:.2f} life {:.3f}s{} center {:.1f} start {:.1f} | EP59 {:.3f} EP63 {:.1f} EP65 {} | "
                         "seeded outcome: {} after {} probe(s), needs {} break(s), {:.1f}s{}",
                         fid, rid, a_ref->GetDisplayFullName() ? a_ref->GetDisplayFullName() : "?",
                         LevelName(j.level), j.level, j.skill, j.picksHeld, j.viaKey ? " (HAS THE KEY)" : "",
                         j.sim.sweet, j.sim.partial, j.sim.life, j.sim.negativeLife ? " (NEGATIVE: no wear, as the engine)" : "",
                         j.sim.center, j.sim.start,
                         s.ep.sweetSpotMult, s.ep.startingArc, s.ep.unbreakable ? 1 : 0,
                         !j.sim.success ? "NO SWEET SPOT" : (enough ? "OPENS" : "OUT OF PICKS"),
                         j.sim.probes, j.sim.broken, j.window, j.viaKey ? " (key)" : "");
            // A KNOWN FAILURE IS NEVER STARTED (coordinator decision pending marth: no picks are
            // spent on a seeded failure). Zero consumption, a standing verdict, a logged reason.
            if (!j.sim.success) {
                g_fail[pairKey] = FailVerdict{ Reason::kNoSweetSpot, kNoPickCountHelps, j.skill };
                LogRefusal(fid, a_ref, Reason::kNoSweetSpot,
                           std::format(" -- sweet spot {:.3f} (EP59 {:.3f}); retried when his skill rises",
                                       j.sim.sweet, s.ep.sweetSpotMult));
                EndJob(fid, nullptr);
                return StepResult::kRefused;
            }
            if (!enough) {
                g_fail[pairKey] = FailVerdict{ Reason::kSimFail, j.sim.broken, j.skill };
                LogRefusal(fid, a_ref, Reason::kSimFail,
                           std::format(" -- the lock needs {} break(s), he holds {} pick(s); nothing spent, retried "
                                       "when he holds more than {} or his skill rises",
                                       j.sim.broken, j.picksHeld, j.sim.broken));
                EndJob(fid, nullptr);
                return StepResult::kRefused;
            }
            // A window that cannot fit the excursion is not started (the cap would end it
            // mid-animation). Longer than the whole cap = never fits: a standing verdict.
            const float cap = Config::g_excursionMax.load();
            if (j.window > left) {
                const Reason why = j.window > cap ? Reason::kTooLong : Reason::kNoTime;
                if (why == Reason::kTooLong) g_fail[pairKey] = FailVerdict{ why, j.picksHeld, j.skill };
                LogRefusal(fid, a_ref, why, std::format(" -- the pick takes {:.1f}s, {:.1f}s left of the {:.0f}s "
                                                        "excursion cap", j.window, left, cap));
                EndJob(fid, nullptr);
                return StepResult::kRefused;
            }
            switch (APMFBridge::ClaimLockpickHold(fid, kIdleLockPick, rid)) {
            case APMFBridge::PickHoldResult::Filed:
                break;
            case APMFBridge::PickHoldResult::Standing:
                // Should not happen: every job end releases its hold (EndJob), including the
                // ended-by-Harbinger path. A leftover means a job was dropped without EndJob --
                // say so, release it, refile once.
                spdlog::warn("[lockpick] {:08X}: a lockpick hold was still filed with no job for it -- released "
                             "and refiled", fid);
                APMFBridge::ReleaseLockpickHold(fid);
                if (APMFBridge::ClaimLockpickHold(fid, kIdleLockPick, rid) == APMFBridge::PickHoldResult::Filed)
                    break;
                [[fallthrough]];
            default:
                LogRefusal(fid, a_ref, Reason::kNoHarbinger, " -- the ch.12 idle v2 claim was not filed");
                EndJob(fid, nullptr);
                return StepResult::kRefused;
            }
            j.holdFiled = true;
            j.phase     = Phase::kPicking;
            j.filedAt   = clk;
            j.liveAt    = -1.0;
            return StepResult::kHold;
        }

        if (j.phase == Phase::kPicking) {
            const auto st = APMFBridge::LockpickHoldStateOf(fid, clk - j.filedAt);
            if (st == APMFBridge::PickHoldState::Ended || st == APMFBridge::PickHoldState::None) {
                // Harbinger ended the idle (not played / refused / follower died): no
                // animation = no pick (principle 7: logged, not masked, nothing unlocked,
                // nothing consumed).
                LogRefusal(fid, a_ref, Reason::kIdleEnded,
                           std::format(" -- the idle claim ended {} (after {} replay(s)); nothing spent",
                                       j.liveAt < 0.0 ? "before it was ever live" : "during the pick window",
                                       j.replays));
                // A standing verdict, so a refused idle is not re-walked to every blocklist
                // cycle: retried when his picks or skill rise (or after a load).
                g_fail[PairKey(fid, rid)] = FailVerdict{ Reason::kIdleEnded, j.picksHeld, j.skill };
                EndJob(fid, nullptr);   // holdFiled stays true: EndJob forgets the bridge's ended entry
                return StepResult::kRefused;
            }
            if (st == APMFBridge::PickHoldState::Pending) return StepResult::kHold;   // not playing yet
            if (j.liveAt < 0.0) {
                j.liveAt   = clk;   // the first play is live: the window starts now
                j.lastPlay = 0.0;
            }
            const double elapsed = clk - j.liveAt;
            if (elapsed < j.window) {
                // ONE replay per clip length from the last play, for the whole window (the clip is
                // one-shot). Never on a pick break: a Repoint mid-clip would cut the clip.
                if (elapsed - j.lastPlay >= kIdleClipSec) {
                    if (APMFBridge::ReplayLockpickIdle(fid)) {
                        j.lastPlay = elapsed;
                        ++j.replays;
                    }
                }
                return StepResult::kHold;
            }

            // The window is over: the world write, in ONE main-thread post that re-validates.
            APMFBridge::ReleaseLockpickHold(fid);
            j.holdFiled = false;
            const int remove = j.viaKey ? 0 : j.sim.broken;
            MainThread::Post([fid, rid, remove]() {
                auto* f = RE::TESForm::LookupByID<RE::Actor>(fid);
                auto* r = RE::TESForm::LookupByID<RE::TESObjectREFR>(rid);
                // RE-VALIDATE on the main thread: the world moved since the worker decided.
                if (!f || f->IsDead()) {
                    spdlog::info("[lockpick] {:08X}: pick of {:08X} SKIPPED at the write -- the follower is gone or "
                                 "dead (nothing removed, not unlocked)", fid, rid);
                    return;
                }
                if (!r || r->IsDisabled() || r->IsMarkedForDeletion() || !r->Is3DLoaded() || !r->IsLocked()) {
                    spdlog::info("[lockpick] {:08X}: pick of {:08X} SKIPPED at the write -- the chest is {} (nothing "
                                 "removed, not unlocked)", fid, rid,
                                 !r ? "gone" : (r->IsDisabled() || r->IsMarkedForDeletion()) ? "disabled or deleted"
                                     : !r->Is3DLoaded() ? "not loaded" : "already unlocked");
                    return;
                }
                if (remove > 0) {
                    const std::int32_t have = CountOf(f, LockpickObject());
                    const std::int32_t n = std::min<std::int32_t>(remove, have);
                    if (n > 0) f->RemoveItem(LockpickObject(), n, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                    if (n < remove)
                        spdlog::warn("[lockpick] {:08X}: {} broken pick(s) to remove, only {} held now", fid, remove, have);
                }
                UnlockOnMain(r);
            });
            j.phase          = Phase::kUnlocking;
            j.unlockPostedAt = clk;
            return StepResult::kHold;
        }

        // kUnlocking: wait for the posted Unlock to show on the ref.
        if (!a_ref->IsLocked()) {
            spdlog::info("[lockpick] {:08X}: {} {:08X} ({} lock){} in {:.1f}s ({} replay(s)) -- transferring", fid,
                         j.viaKey ? "UNLOCKED with the key" : "PICKED", rid, LevelName(j.level),
                         j.viaKey ? "" : std::format(", broke {} pick(s)", j.sim.broken), j.window, j.replays);
            g_openedByUs[fid] = rid;   // LP-M2: the door leg's F-L3 check reads it once
            EndJob(fid, nullptr);
            return StepResult::kProceed;
        }
        if (clk - j.unlockPostedAt < kUnlockFloorSec) return StepResult::kHold;
        spdlog::error("[lockpick] {:08X}: the engine Unlock was posted for {:08X} {:.0f}s of running game ago and "
                      "the lock still reads locked (the post may have skipped: see its line) -- attempt dropped",
                      fid, rid, kUnlockFloorSec);
        g_fail[PairKey(fid, rid)] = FailVerdict{ Reason::kUnlockFailed, j.picksHeld, j.skill };
        EndJob(fid, nullptr);
        return StepResult::kRefused;
    }

    void SweepStale(Clock::time_point /*a_now*/) {
        const double clk = Scheduler::ServiceClock();   // UNPAUSED: a menu never ages a pick
        for (auto it = g_jobs.begin(); it != g_jobs.end();) {
            const RE::FormID fid = it->first;
            const Job& j = it->second;
            // Still driven? His loot slot must still target this lock, and the arrival
            // step must have run within kStaleFloorSec of running game.
            const TravelIntent* tr = SlotOf(fid);
            RE::FormID slotRef = 0;
            if (tr) {
                auto tp = tr->target.get();
                slotRef = tp ? tp->GetFormID() : 0;
            }
            const char* why = nullptr;
            if (!tr || slotRef != j.ref)             why = "the loot excursion ended or moved on";
            else if (clk - j.lastStep > kStaleFloorSec) why = "the arrival step stopped running";
            if (!why) { ++it; continue; }
            ++it;   // EndJob erases fid's entry
            EndJob(fid, why);
        }
    }

    bool HasJob(RE::FormID a_follower, RE::FormID a_ref) {
        const auto it = g_jobs.find(a_follower);
        return it != g_jobs.end() && it->second.ref == a_ref;
    }

    bool ConsumeOpenedByUs(RE::FormID a_follower, RE::FormID a_ref) {
        const auto it = g_openedByUs.find(a_follower);
        if (it == g_openedByUs.end() || it->second != a_ref) return false;
        g_openedByUs.erase(it);
        return true;
    }

    bool IsDoor(const RE::TESObjectREFR* a_ref) {
        auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
        return base && base->Is(RE::FormType::Door);
    }

    // ── LP-M2: a locked DOOR at a GATED block (see the declaration) ─────────────────
    // Which door: a LOCKED, enabled DOOR ref within kGateDoorReach of the block point S, ON THE
    // WAY from S to the target T: its xy distance to the S->T line is at most kGateDoorLane
    // (about one door width) and it is not behind S (its projection on S->T is at least
    // -kGateDoorAtS). Nearest to S first. Searched in the follower's and the target's ATTACHED
    // parent cells (#72: the cell's own locked walk, read-only; the judge runs after the walk).
    // kGateDoorReach: he stalls pressing into the door, so its reference (at the door's center)
    // sits within a door's width of S; 256 u is about two door widths.
    // THE LEG AIMS AT A POINT, NOT AT THE DOOR (F-L3, tier-A review of c6fc73d): kDoorStandOff
    // back from the door toward S, at S's floor height, so the path never enters the door's
    // portal (the engine's NPC TESObjectDOOR Activate unlocks with no skill check when the NPC
    // holds a lockpick). ch.19 point legs only (RetargetExcursionLeg refuses a point on the
    // MFO-package road, logged here); the arrival step still targets the DOOR ref.
    bool DispatchGateDoor(RE::Actor* a_follower, int a_slot, RE::TESObjectREFR* a_target,
                          const RE::NiPoint3& a_blockPos, Clock::time_point a_now) {
        constexpr float kGateDoorReach = 256.0f;
        constexpr float kGateDoorAtS   = 64.0f;
        constexpr float kGateDoorLane  = 128.0f;   // ~one door width off the S->T line
        constexpr float kDoorStandOff  = 96.0f;
        if (!a_follower || !a_target || IsDoor(a_target)) return false;   // no door-behind-a-door chains
        if (a_slot < 0 || a_slot >= Packages::kMaxLootSlots) return false;
        if (InertReason() != Reason::kNone) return false;
        const RE::FormID fid = a_follower->GetFormID();
        const RE::NiPoint3 S = a_blockPos;
        const RE::NiPoint3 T = a_target->GetPosition();
        float tx = T.x - S.x, ty = T.y - S.y;
        const float lt = std::sqrt(tx * tx + ty * ty);
        if (lt < 1.0f) return false;   // no direction to judge "on the way" by
        tx /= lt;
        ty /= lt;
        RE::ObjectRefHandle best;
        float bestD = kGateDoorReach + 1.0f;
        RE::TESObjectCELL* cells[2] = { a_follower->GetParentCell(), a_target->GetParentCell() };
        if (cells[1] == cells[0]) cells[1] = nullptr;
        for (auto* c : cells) {
            if (!c || !c->IsAttached()) continue;
            c->ForEachReferenceInRange(S, kGateDoorReach, [&](RE::TESObjectREFR& r) {
                auto* base = r.GetBaseObject();
                if (!base || !base->Is(RE::FormType::Door) || r.IsDisabled() || r.IsMarkedForDeletion())
                    return RE::BSContainer::ForEachResult::kContinue;
                if (!r.IsLocked()) return RE::BSContainer::ForEachResult::kContinue;
                const RE::NiPoint3 P = r.GetPosition();
                const float d = P.GetDistance(S);
                if (d > kGateDoorReach || d >= bestD) return RE::BSContainer::ForEachResult::kContinue;
                const float px = P.x - S.x, py = P.y - S.y;
                const float along = px * tx + py * ty;                  // projection on S->T
                const float perp  = std::fabs(px * ty - py * tx);       // distance off the S->T line
                if (along < -kGateDoorAtS || perp > kGateDoorLane)
                    return RE::BSContainer::ForEachResult::kContinue;   // behind him, or off the way
                bestD = d;
                best  = r.GetHandle();
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        auto  dptr = best.get();
        auto* door = dptr.get();
        if (!door) {
            spdlog::info("[lockpick] {:08X}: GATED on the way to {:08X} with no locked door within {:.0f} u of the "
                         "block and {:.0f} u of the way -- a gate, not a lock (M1's re-admit applies)", fid,
                         a_target->GetFormID(), kGateDoorReach, kGateDoorLane);
            return false;
        }
        if (!Admit(a_follower, door, a_now)) return false;   // logged once, with its reason
        // The stand-off point: kDoorStandOff from the door toward S (xy), at S's floor.
        const RE::NiPoint3 D = door->GetPosition();
        float bx = S.x - D.x, by = S.y - D.y;
        const float lb = std::sqrt(bx * bx + by * by);
        if (lb > 1.0f) { bx /= lb; by /= lb; } else { bx = -tx; by = -ty; }
        const RE::NiPoint3 point{ D.x + bx * kDoorStandOff, D.y + by * kDoorStandOff, S.z };
        const TravelIntent& tr = g_travelSlots[a_slot];
        const float df = a_follower->GetPosition().GetDistance(point);
        if (!RetargetExcursionLeg(a_follower, a_slot, door, tr.cat, tr.want, df, a_now, &point)) {
            spdlog::info("[lockpick] {:08X}: locked door {:08X} blocks {:08X} but the door leg did not dispatch (it "
                         "needs Harbinger's ch.19 road with ABI >= 11 so he walks to a point IN FRONT of the door, "
                         "never into it; this excursion is on {}) -- the item stays GATED", fid, door->GetFormID(),
                         a_target->GetFormID(),
                         APMFBridge::HasLootTravelLeg(a_slot) ? "an APMF below v11" : "MFO's own travel package");
            return false;
        }
        spdlog::info("[lockpick] {:08X}: {:08X} is GATED by LOCKED DOOR {:08X} '{}' ({:.0f} u from the block) -- door "
                     "leg dispatched to a point {:.0f} u in front of it ({:.0f} u from him): pick it, unlock it, and "
                     "its lock-changed event re-admits the gate", fid, a_target->GetFormID(), door->GetFormID(),
                     door->GetDisplayFullName() ? door->GetDisplayFullName() : "?", bestD, kDoorStandOff, df);
        return true;
    }

    void Abort(RE::FormID a_follower, const char* a_why) {
        if (g_jobs.contains(a_follower)) EndJob(a_follower, a_why);
    }

    void Clear() {
        APMFBridge::ReleaseAllLockpickHolds();
        g_jobs.clear();
        g_fail.clear();
        g_logged.clear();
        g_openedByUs.clear();
        {
            std::scoped_lock lk(g_snapMx);
            g_snaps.clear();
        }
    }

}
