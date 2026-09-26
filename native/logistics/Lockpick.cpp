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
//     settings, picks, key) -> the seeded SIMULATION -> Harbinger ch.1 stand-still + ch.12 v2
//     IdleLockPick at the lock for the simulated seconds (one Repoint per simulated pick
//     break) -> on success ONE main-thread post that removes the broken picks from the
//     FOLLOWER's inventory and calls the engine's own Unlock (bound by id, verified row) ->
//     the lock reads unlocked -> the existing transfer (StripCorpse) runs. On failure the
//     same post removes every pick used and the lock stays locked.
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
//            = an instant break; a break costs one lockpick and resets health to 100;
//   success = the lock reaches fLockMaxAngle, then fUnlockDoorDelay seconds.
// The SEARCHER is the one modelled (non-engine) part: it sweeps from the start angle in steps
// of the partial window until the lock gives, then narrows (pattern search on how far the lock
// turned). It holds a binding lock for kBindReactSec before letting go. Seeded per
// (follower, lock) so a re-issue of the same lock replays the same outcome.
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
        // Pattern-search bounds: a sweet spot of 0 (an EP59 of 0) never opens, so the
        // search must end. A cap on the number of probes and a floor on the step are the
        // only exits besides success and running out of picks.
        constexpr int   kProbeCap  = 512;
        constexpr float kMinStepDeg = 0.001f;

        // A snapshot that never comes back (the main-thread pump stalled) ends the attempt
        // loudly after this long. A FLOOR: the pump drains every frame.
        constexpr auto kSnapshotFloor = std::chrono::seconds(5);
        // A posted Unlock that has not shown up on the ref after this long is a failure,
        // logged, never retried in place. A floor (one frame is the real latency).
        constexpr auto kUnlockFloor = std::chrono::seconds(3);
        // A pick job whose arrival step has not run for this long is abandoned (the caller
        // stopped driving it: a courtesy hold, a leash, a teleport). Three logistics ticks
        // (kLogisticsInterval is 1 s), so a live job is never aged out. Abandoning releases
        // the hold and consumes nothing.
        constexpr auto kStaleFloor = std::chrono::seconds(3);
        // Unpaused pick clock: one step of it is capped here (the logistics cadence is 1 s;
        // a longer gap is a pause or a load, not picking time).
        constexpr float kMaxClockStep = 2.0f;

        enum class Reason : std::uint8_t {
            kNone, kOwned, kOffLimits, kNotChest, kRequiresKey, kNoPicks, kCreature,
            kNoHarbinger, kNoMainThread, kUnlockSeat, kSimFail, kTooLong, kNotLocked,
            kGone, kSettings, kIdleEnded, kUnlockFailed, kNoTime,
        };
        const char* ReasonName(Reason a_r) {
            switch (a_r) {
            case Reason::kNone:         return "none";
            case Reason::kOwned:        return "owned";
            case Reason::kOffLimits:    return "offlimits";
            case Reason::kNotChest:     return "notChest";
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
            int      picks = 0;
            Settings s{};
            std::uint64_t seed = 0;
        };
        struct SimOut {
            bool   success = false;
            int    broken = 0;
            float  seconds = 0.0f;
            int    probes = 0;
            bool   probeCap = false;
            float  sweet = 0, partial = 0, life = 0, center = 0, start = 0;
            std::vector<float> breakAt;   // window-relative seconds of each simulated break
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
            const float half = o.sweet * 0.5f;
            const float lo = half - s.pickMax, hi = s.pickMax - half;
            o.center = lo + rng.U() * (hi - lo);
            float p0 = 0.0f;
            if (a_in.ep63 > 0.0f) {
                const float a = std::max(o.center - a_in.ep63, lo);
                const float b = std::min(o.center + a_in.ep63, hi);
                p0 = a + rng.U() * (b - a);
            }
            o.start = p0;

            auto allowed = [&](float a_p) {
                const float d = std::fabs(a_p - o.center);
                if (d <= half) return s.lockMax;
                if (d > half + o.partial) return 0.0f;
                return (1.0f - (d - half) / o.partial) * s.lockMax;
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
                if (!a_in.unbreakable) {
                    if (o.life <= 0.0f) {         // the engine's zero-life branch: an instant break
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
                if (!a_in.unbreakable && health <= 0.0f) {
                    ++o.broken;
                    o.breakAt.push_back(t);
                    health = 100.0f;
                    if (o.broken >= a_in.picks) done = true;
                }
                t += A / (2.0f * s.rotSpeed);     // spring back at twice the turn speed
                return A;
            };
            auto clampP = [&](float a_p) { return std::clamp(a_p, -s.pickMax, s.pickMax); };

            // Phase A: sweep outward from the start in steps of the partial window. Coverage:
            // any center lies within step/2 of a probe, inside the felt window (half+partial).
            const float step = o.partial > 0.0f ? o.partial : std::max(o.sweet, 1.0f);
            const float dir0 = (rng.Next() & 1ull) ? 1.0f : -1.0f;
            float best = 0.0f, bestA = -1.0f;
            for (int k = 0; !done && o.probes < kProbeCap; ++k) {
                bool any = false;
                for (int side = 0; side < (k == 0 ? 1 : 2) && !done; ++side) {
                    const float off = static_cast<float>(k) * step * (side == 0 ? dir0 : -dir0);
                    const float p   = p0 + off;
                    if (p < -s.pickMax || p > s.pickMax) continue;
                    any = true;
                    const float A = probe(p);
                    if (A > 0.0f && A > bestA) { best = p; bestA = A; }
                }
                if (bestA > 0.0f || !any) break;
            }
            // Phase B: pattern search on how far the lock turned, halving the step.
            float h   = std::max(o.partial * 0.5f, half);
            float dir = dir0;
            while (!done && bestA > 0.0f && o.probes < kProbeCap && h >= kMinStepDeg) {
                const float p1 = clampP(best + dir * h);
                const float A1 = probe(p1);
                if (done) break;
                if (A1 > bestA) { best = p1; bestA = A1; continue; }
                const float p2 = clampP(best - dir * h);
                const float A2 = probe(p2);
                if (done) break;
                if (A2 > bestA) { best = p2; bestA = A2; dir = -dir; continue; }
                h *= 0.5f;
            }
            if (!done) o.probeCap = true;   // no sweet spot to find (e.g. EP59 = 0) or cap hit
            o.seconds = t;
            return o;
        }

        // ── per-follower state (worker-only) ──────────────────────────────────────
        std::uint64_t PairKey(RE::FormID a_f, RE::FormID a_r) {
            return (static_cast<std::uint64_t>(a_f) << 32) | a_r;
        }
        // A standing failed verdict: the lock is refused for this follower until his
        // lockpick count or his skill rises above what the verdict was reached with.
        struct FailVerdict { Reason why = Reason::kNone; std::int32_t picks = 0; float skill = 0.0f; };
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
        struct Job {
            RE::FormID    ref = 0;
            Phase         phase = Phase::kSnapshot;
            std::uint64_t ticket = 0;
            Clock::time_point startedAt{}, lastStep{}, unlockPostedAt{};
            bool          viaKey = false;
            bool          holdFiled = false;
            float         clock = 0.0f;        // unpaused pick-window seconds elapsed
            float         window = 0.0f;       // the simulated seconds
            std::size_t   nextBreak = 0;
            int           level = 0;
            float         skill = 0.0f;
            std::int32_t  picksHeld = 0;
            SimOut        sim{};
        };
        std::unordered_map<RE::FormID, Job> g_jobs;

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
        if (!base || !base->Is(RE::FormType::Container)) { LogRefusal(fid, a_ref, Reason::kNotChest, ""); return false; }
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
            j.startedAt = a_now;
            j.lastStep  = a_now;
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
        // Unpaused pick clock (the caller runs only on unpaused services).
        const float dt = std::chrono::duration<float>(a_now - j.lastStep).count();
        j.lastStep = a_now;

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
                if (a_now - j.startedAt < kSnapshotFloor) return StepResult::kHold;
                spdlog::error("[lockpick] {:08X}: the main-thread snapshot for {:08X} never came back ({}s) -- "
                              "attempt dropped", fid, rid,
                              std::chrono::duration_cast<std::chrono::seconds>(kSnapshotFloor).count());
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
                in.picks       = s.picks;
                in.s           = s.s;
                in.seed        = PairKey(fid, rid) ^ 0x4C4F434B5049434Bull;   // "LOCKPICK"
                j.sim = Simulate(in);
            }
            j.window = j.sim.seconds;
            const float left = std::chrono::duration<float>(a_excursionEnd - a_now).count();
            spdlog::info("[lockpick] {:08X}: {:08X} '{}' {} lock ({}), skill {:.0f}, picks {}{} | sweet {:.2f} "
                         "partial {:.2f} life {:.3f}s center {:.1f} start {:.1f} | EP59 {:.3f} EP63 {:.1f} EP65 {} | "
                         "-> {} after {} probe(s), {} broken, {:.1f}s{}",
                         fid, rid, a_ref->GetDisplayFullName() ? a_ref->GetDisplayFullName() : "?",
                         LevelName(j.level), j.level, j.skill, j.picksHeld, j.viaKey ? " (HAS THE KEY)" : "",
                         j.sim.sweet, j.sim.partial, j.sim.life, j.sim.center, j.sim.start,
                         s.ep.sweetSpotMult, s.ep.startingArc, s.ep.unbreakable ? 1 : 0,
                         j.sim.success ? "SUCCESS" : (j.sim.probeCap ? "FAIL (no sweet spot found)" : "FAIL (out of picks)"),
                         j.sim.probes, j.sim.broken, j.window, j.viaKey ? " (key)" : "");
            // A window that cannot fit the excursion is not started (the cap would end it
            // mid-animation). Longer than the whole cap = never fits: a standing verdict.
            const float cap = Config::g_excursionMax.load();
            if (j.window > left) {
                const Reason why = j.window > cap ? Reason::kTooLong : Reason::kNoTime;
                if (why == Reason::kTooLong) g_fail[PairKey(fid, rid)] = FailVerdict{ why, j.picksHeld, j.skill };
                LogRefusal(fid, a_ref, why, std::format(" -- the pick takes {:.1f}s, {:.1f}s left of the {:.0f}s "
                                                        "excursion cap", j.window, left, cap));
                EndJob(fid, nullptr);
                return StepResult::kRefused;
            }
            switch (APMFBridge::ClaimLockpickHold(fid, kIdleLockPick, rid)) {
            case APMFBridge::PickHoldResult::Filed:
                break;
            case APMFBridge::PickHoldResult::Standing:
                // A hold MFO no longer tracks as a job (cannot normally happen: jobs and
                // holds end together). Release it and refile once.
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
            j.clock     = 0.0f;
            j.nextBreak = 0;
            return StepResult::kHold;
        }

        if (j.phase == Phase::kPicking) {
            const auto st = APMFBridge::LockpickHoldStateOf(fid);
            if (st == APMFBridge::PickHoldState::Ended || st == APMFBridge::PickHoldState::None) {
                // Harbinger ended the idle (not played / refused / follower died): no
                // animation = no pick (principle 7: logged, not masked, nothing unlocked).
                LogRefusal(fid, a_ref, Reason::kIdleEnded, " -- the idle claim ended before the pick window did");
                // A standing verdict, so a refused idle is not re-walked to every blocklist
                // cycle: retried when his picks or skill rise (or after a load).
                g_fail[PairKey(fid, rid)] = FailVerdict{ Reason::kIdleEnded, j.picksHeld, j.skill };
                j.holdFiled = false;   // the bridge already released it
                EndJob(fid, nullptr);
                return StepResult::kRefused;
            }
            if (st == APMFBridge::PickHoldState::Pending) return StepResult::kHold;   // not playing yet
            j.clock += std::clamp(dt, 0.0f, kMaxClockStep);
            // One replay per simulated pick break (a declared event, never a timer): the
            // clip is one-shot, a new pick starts a new play.
            bool replay = false;
            while (j.nextBreak < j.sim.breakAt.size() && j.sim.breakAt[j.nextBreak] <= j.clock) {
                ++j.nextBreak;
                replay = true;
            }
            if (replay && j.clock < j.window) APMFBridge::ReplayLockpickIdle(fid);
            if (j.clock < j.window) return StepResult::kHold;

            // The window is over: the world write, in ONE main-thread post.
            APMFBridge::ReleaseLockpickHold(fid);
            j.holdFiled = false;
            const int  remove = j.viaKey ? 0 : j.sim.broken;
            const bool unlock = j.sim.success;
            MainThread::Post([fid, rid, remove, unlock]() {
                auto* f = RE::TESForm::LookupByID<RE::Actor>(fid);
                auto* r = RE::TESForm::LookupByID<RE::TESObjectREFR>(rid);
                if (f && remove > 0) {
                    const std::int32_t have = CountOf(f, LockpickObject());
                    const std::int32_t n = std::min<std::int32_t>(remove, have);
                    if (n > 0) f->RemoveItem(LockpickObject(), n, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                    if (n < remove)
                        spdlog::warn("[lockpick] {:08X}: {} broken pick(s) to remove, only {} held now", fid, remove, have);
                }
                if (unlock && r) UnlockOnMain(r);
            });
            if (!unlock) {
                g_fail[PairKey(fid, rid)] = FailVerdict{ Reason::kSimFail, j.picksHeld - remove, j.skill };
                spdlog::info("[lockpick] {:08X}: FAILED {:08X} ({} lock) -- broke {} pick(s) in {:.1f}s, the lock "
                             "stays locked", fid, rid, LevelName(j.level), remove, j.window);
                EndJob(fid, nullptr);
                return StepResult::kRefused;
            }
            j.phase          = Phase::kUnlocking;
            j.unlockPostedAt = a_now;
            return StepResult::kHold;
        }

        // kUnlocking: wait for the posted Unlock to show on the ref.
        if (!a_ref->IsLocked()) {
            spdlog::info("[lockpick] {:08X}: {} {:08X} ({} lock){} in {:.1f}s -- transferring", fid,
                         j.viaKey ? "UNLOCKED with the key" : "PICKED", rid, LevelName(j.level),
                         j.viaKey ? "" : std::format(", broke {} pick(s)", j.sim.broken), j.window);
            EndJob(fid, nullptr);
            return StepResult::kProceed;
        }
        if (a_now - j.unlockPostedAt < kUnlockFloor) return StepResult::kHold;
        spdlog::error("[lockpick] {:08X}: the engine Unlock was posted for {:08X} {}s ago and the lock still "
                      "reads locked -- attempt dropped (picks already removed)", fid, rid,
                      std::chrono::duration_cast<std::chrono::seconds>(kUnlockFloor).count());
        g_fail[PairKey(fid, rid)] = FailVerdict{ Reason::kUnlockFailed, j.picksHeld, j.skill };
        EndJob(fid, nullptr);
        return StepResult::kRefused;
    }

    void SweepStale(Clock::time_point a_now) {
        for (auto it = g_jobs.begin(); it != g_jobs.end();) {
            const RE::FormID fid = it->first;
            const Job& j = it->second;
            // Still driven? His loot slot must still target this lock, and the arrival
            // step must have run recently.
            const TravelIntent* tr = SlotOf(fid);
            RE::FormID slotRef = 0;
            if (tr) {
                auto tp = tr->target.get();
                slotRef = tp ? tp->GetFormID() : 0;
            }
            const char* why = nullptr;
            if (!tr || slotRef != j.ref)             why = "the loot excursion ended or moved on";
            else if (a_now - j.lastStep > kStaleFloor) why = "the arrival step stopped running";
            if (!why) { ++it; continue; }
            ++it;   // EndJob erases fid's entry
            EndJob(fid, why);
        }
    }

    void Abort(RE::FormID a_follower, const char* a_why) {
        if (g_jobs.contains(a_follower)) EndJob(a_follower, a_why);
    }

    void Clear() {
        APMFBridge::ReleaseAllLockpickHolds();
        g_jobs.clear();
        g_fail.clear();
        g_logged.clear();
        {
            std::scoped_lock lk(g_snapMx);
            g_snaps.clear();
        }
    }

}
