// apmf/TauntClaims.cpp -- the Harbinger claims of the TAUNT gambit (taunt slice S2, ClickUp 86e3g6duw,
// Docs/TAUNT-DESIGN.md). The decisions (who, when, the roll, when to release) are native/Taunt.cpp's; this file
// owns only the claims and their lifecycle, in the two shapes Harbinger's FACETS.md gives:
//
//   * the TAUNTER's idle -- ch.12 kIntent_Idle v2 (form = the combat taunt IDLE record, target = the foe the idle is
//     played at), then ch.1 kIntent_MovementBlock (hold him still, no param) filed ONLY once the idle claim first
//     reads LIVE (Harbinger accepted the play; a refused idle ends the claim and the follower is never held). ABI v17. Keyed by the
//     taunter. Same pair as the LOTD deposit (apmf/Deposit.cpp) and the lockpick hold.
//   * the FOE's pin -- FACETS "Pinning a hostile NPC (e.g. a taunt)", Example 20a. ch.20 kIntent_TargetPin on
//     the FOE with form = the TAUNTER (fval 0, ival 0), plus a ch.21 kIntent_CombatEntry on the foe naming
//     the taunter ONLY when the taunter is not already a target of the foe's combat group (fval 0, so the
//     range rule never applies to a hostile holder). Keyed by the FOE: one pin per foe, a second taunter
//     REPOINTS it. An ended handle (IsClaimLive was true, now false) is released and re-requested, never
//     repointed; one that has never read live is PENDING and may still be repointed (Example 20a's Ended()).
//
// ABI. Idle v2 is v17 and an OLDER Harbinger does not refuse a form (it plays the v1 idle), so nothing here is
// requested below v24 (GetIdleState, the idle verdict). v24 implies idle v2 (17), the pin (13), the entry (14) and IsClaimLive / Repoint (6).
//
// THREADING. Called from the AddTask job worker (Scheduler tick -> Taunt::Tick / Fire) and from a
// MainThread::Post'd task (Taunt's roll-and-pin step). APMF's Request / Repoint / Release / IsClaimLive are
// safe from any thread (APMF_API.h) and never call back into MFO, so they are leaf calls under the own mutex
// s_mx (never g_mx; this file never takes g_mx). NOTHING IS SAVED: APMF drops every claim at a load, and
// ClearTauntClaims drops the record of them (a stale Release is a harmless no-op).
#include "APMFBridge.h"
#include "APMFBridge_internal.h"   // g_apmf, kOwnBasis
#include "APMF_API.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kTauntMinAbi = 24;   // GetIdleState (implies ch.12 idle v2 = 17, pin 13, entry 14)

        // The idle record is created at RequestEx; this wall-clock grace only covers the instant before the call is
        // visible. A state still Unknown after it is a failed attempt. It judges nothing about whether the idle played.
        constexpr auto kIdleRecordGrace = std::chrono::milliseconds(1500);

        struct IdleClaims {
            APMF_API::Handle hold = APMF_API::kInvalidHandle;
            APMF_API::Handle idle = APMF_API::kInvalidHandle;
            std::chrono::steady_clock::time_point filed{};
            bool             holdTried = false;   // the ch.1 hold is filed ONCE, when the idle is first seen live
        };
        struct FoeClaims {
            APMF_API::Handle pin = APMF_API::kInvalidHandle, entry = APMF_API::kInvalidHandle;
            bool             pinSeen = false, entrySeen = false;   // has IsClaimLive ever read true for it?
            RE::FormID       taunter = 0;                           // whom the foe is pinned to now
        };
        std::mutex                                   s_mx;
        std::unordered_map<RE::FormID, IdleClaims>   s_idles;   // keyed by TAUNTER; guarded by s_mx
        std::unordered_map<RE::FormID, FoeClaims>    s_foes;    // keyed by FOE; guarded by s_mx
        // A synchronous refusal of a valid pin / idle = the seat is not installed (VR, runtime, [TargetPin]
        // bTargetPin=0, [Idle] bIdleV2=0, self-check). Session-stable (seats install once at APMF's kDataLoaded).
        std::atomic<bool> s_seatRefused{ false };

        const APMF_API::APMF_API_v24* TauntApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kTauntMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v24*>(api);   // v24 >= v6: Repoint + IsClaimLive, plus GetIdleState
        }

        // Example 20a's Ended(): a handle that WAS live and is not any more has ended (Harbinger ended it, or a
        // load did). One that has never read live is pending, not ended. s_mx HELD.
        bool Ended(const APMF_API::APMF_API_v24* a_api, APMF_API::Handle a_h, bool& a_seen) {
            if (a_h == APMF_API::kInvalidHandle) return false;
            const bool live = a_api->IsClaimLive(a_h);
            if (live) a_seen = true;
            return a_seen && !live;
        }

        void ReleaseIdleLocked(const APMF_API::APMF_API_v2* a_api, IdleClaims& a_c) {
            // Idle first (its guarded reset runs only if the idle is still HELD), then the hold.
            for (auto* h : { &a_c.idle, &a_c.hold }) {
                if (a_api && *h != APMF_API::kInvalidHandle) a_api->Release(*h);
                *h = APMF_API::kInvalidHandle;
            }
        }
        void ReleaseFoeLocked(const APMF_API::APMF_API_v2* a_api, FoeClaims& a_c) {
            // Pin first (the foe's own selector answers again), then the entry (stops nothing).
            for (auto* h : { &a_c.pin, &a_c.entry }) {
                if (a_api && *h != APMF_API::kInvalidHandle) a_api->Release(*h);
                *h = APMF_API::kInvalidHandle;
            }
        }
    }

    bool TauntSupported() { return TauntApi() != nullptr && !s_seatRefused.load(std::memory_order_relaxed); }

    bool ClaimTauntIdle(RE::FormID a_taunter, RE::FormID a_idle, RE::FormID a_target) {
        const auto* api = TauntApi();
        if (!api || a_taunter == 0 || a_idle == 0 || a_target == 0 || a_target == a_taunter) return false;
        std::scoped_lock lock(s_mx);
        if (s_idles.contains(a_taunter)) return true;   // one attempt at a time (Taunt.cpp never double-files)
        IdleClaims c{};

        // NO ch.1 hold yet (marth 2026-10-13): the hold is filed by ReadTauntIdle the first time the idle claim reads LIVE,
        // i.e. once Harbinger has ACCEPTED the play (PlayIdle succeeded). A refused idle (its conditions fail) ends the claim
        // and costs the follower nothing: he keeps fighting.
        APMF_API::APMF_Param p{};
        p.form   = a_idle;     // the IDLE record (v17: an older APMF would ignore it -- gated above)
        p.target = a_target;   // the foe: the engine evaluates the idle's conditions against it
        c.idle = api->RequestEx(a_taunter, APMF_API::kIntent_Idle, kOwnBasis, &p);
        if (c.idle == APMF_API::kInvalidHandle) {
            if (!s_seatRefused.exchange(true))
                spdlog::warn("[taunt] {:08X}: Harbinger REFUSED the ch.12 idle v2 claim (idle {:08X} at {:08X}) at the "
                             "call: the idle seat is not installed ([Idle] bIdleV2=0 in APMF.ini, VR, a runtime other "
                             "than 1.6.1170 / 1.5.97, or its address self-check; APMF's log names it). Taunt is INERT "
                             "for this session. MFO has no direct road.", a_taunter, a_idle, a_target);
            return false;
        }
        c.filed = std::chrono::steady_clock::now();
        s_idles.emplace(a_taunter, c);
        return true;
    }

    namespace {
        const char* IdleStateName(std::uint32_t s) {
            switch (s) {
                case APMF_API::kIdleState_Unknown: return "Unknown";
                case APMF_API::kIdleState_Pending: return "Pending";
                case APMF_API::kIdleState_Accepted: return "Accepted";
                case APMF_API::kIdleState_Confirmed: return "Confirmed";
                case APMF_API::kIdleState_ConfirmedWeak: return "ConfirmedWeak";
                case APMF_API::kIdleState_NotConfirmed: return "NotConfirmed";
                case APMF_API::kIdleState_NoTransition: return "NoTransition";
                case APMF_API::kIdleState_Refused: return "Refused";
                case APMF_API::kIdleState_Unconfirmable: return "Unconfirmable";
                case APMF_API::kIdleState_NotPlayed: return "NotPlayed";
                case APMF_API::kIdleState_Carried: return "Carried";
                default: return "?";
            }
        }
        const char* EndName(std::uint32_t e) {
            switch (e) {
                case APMF_API::kIdleEnd_None: return "None";
                case APMF_API::kIdleEnd_Harbinger: return "Harbinger";
                case APMF_API::kIdleEnd_ActorDied: return "ActorDied";
                case APMF_API::kIdleEnd_Released: return "Released";
                case APMF_API::kIdleEnd_Superseded: return "Superseded";
                case APMF_API::kIdleEnd_Gone: return "Gone";
                default: return "?";
            }
        }
    }

    // The FACETS ch.12 client example, branch for branch (Docs/FACETS.md "Reading what happened (ABI 24)").
    TauntIdleReading ReadTauntIdle(RE::FormID a_taunter) {
        TauntIdleReading out;
        const auto* api = TauntApi();
        if (!api || !api->GetIdleState) return out;
        std::scoped_lock lock(s_mx);
        const auto it = s_idles.find(a_taunter);
        if (it == s_idles.end() || it->second.idle == APMF_API::kInvalidHandle) return out;
        auto& c = it->second;
        APMF_API::APMF_IdleStateInfo info{};
        info.size = sizeof(info);
        const std::uint32_t st = api->GetIdleState(c.idle, &info);
        out.msToEntry = info.msToEntry;
        out.pending = st == APMF_API::kIdleState_Pending && info.endCause == APMF_API::kIdleEnd_None;
        out.detail = std::string("state=") + IdleStateName(st) + " endCause=" + EndName(info.endCause);
        if (info.unconfirmableWhy) out.detail += " unconfirmableWhy=" + std::to_string(info.unconfirmableWhy);
        using V = TauntIdleVerdict;
        if (info.endCause == APMF_API::kIdleEnd_Superseded) {
            // Another claim owns ch.12 on the actor: ours is live but not played. A failed attempt (never wait on it).
            out.detail += " (another claim owns ch.12 on him)";
            out.verdict = V::Failed;
        } else if (st == APMF_API::kIdleState_Confirmed || st == APMF_API::kIdleState_ConfirmedWeak) {
            out.verdict = V::Played;
            out.weak = st == APMF_API::kIdleState_ConfirmedWeak;
        } else if (info.endCause != APMF_API::kIdleEnd_None) {
            out.verdict = V::Failed;   // ended without playing (NotConfirmed, NoTransition, Refused, NotPlayed, Released, Gone, ActorDied)
        } else if (st == APMF_API::kIdleState_Unknown) {
            if (std::chrono::steady_clock::now() - c.filed < kIdleRecordGrace) out.verdict = V::Waiting;   // record not visible yet
            else out.verdict = V::Failed;
        } else if (st == APMF_API::kIdleState_Unconfirmable || st == APMF_API::kIdleState_Carried) {
            out.verdict = V::Failed;   // live, but no evidence for this play: never act on it as played
        } else {
            out.verdict = V::Waiting;  // Pending or Accepted: Harbinger judges the window, MFO does not time it
        }
        // The ch.1 hold, ONCE, as soon as the play is ACCEPTED (PlayIdle returned true), seen as Accepted. A first read that is
        // already Confirmed files none (the clip is over by the time the roll runs). A refused idle never reaches here, so it never holds him.
        if (!c.holdTried && st == APMF_API::kIdleState_Accepted) {
            c.holdTried = true;
            APMF_API::APMF_Param hp{};
            c.hold = api->RequestEx(a_taunter, APMF_API::kIntent_MovementBlock, kOwnBasis, &hp);
            if (c.hold == APMF_API::kInvalidHandle)
                spdlog::warn("[taunt] {:08X}: ch.1 hold REFUSED by Harbinger -- the taunt idle plays without a "
                             "stand-still (APMF.log names the reason)", a_taunter);
        }
        return out;
    }

    void ReleaseTauntIdle(RE::FormID a_taunter) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_mx);
        const auto it = s_idles.find(a_taunter);
        if (it == s_idles.end()) return;
        ReleaseIdleLocked(api, it->second);
        s_idles.erase(it);
    }

    TauntPinResult PinFoeToTaunter(RE::FormID a_foe, RE::FormID a_taunter, bool a_taunterInFoeGroup) {
        const auto* api = TauntApi();
        if (!api || s_seatRefused.load(std::memory_order_relaxed)) return TauntPinResult::Refused;
        // APMF refuses these synchronously too; filtering them keeps a synchronous refusal meaning ONE thing.
        if (a_foe == 0 || a_taunter == 0 || a_foe == a_taunter || a_foe == 0x14) return TauntPinResult::Refused;

        std::scoped_lock lock(s_mx);
        APMF_API::APMF_Param p{};
        p.form = a_taunter;   // the foe's new target
        p.fval = 0.0f;        // no engage range on a hostile holder (FACETS Table 23a)
        auto& t = s_foes[a_foe];
        // An ended handle is never repointed: release it and request a fresh claim below.
        if (Ended(api, t.entry, t.entrySeen)) {
            api->Release(t.entry); t.entry = APMF_API::kInvalidHandle; t.entrySeen = false;
        }
        if (Ended(api, t.pin, t.pinSeen)) {
            api->Release(t.pin); t.pin = APMF_API::kInvalidHandle; t.pinSeen = false;
        }
        const bool repoint = t.pin != APMF_API::kInvalidHandle;

        if (t.entry != APMF_API::kInvalidHandle) api->Repoint(t.entry, &p);   // one more StartCombat, for this taunter
        else if (!a_taunterInFoeGroup)
            t.entry = api->RequestEx(a_foe, APMF_API::kIntent_CombatEntry, kOwnBasis, &p);

        if (repoint) {
            api->Repoint(t.pin, &p);   // already taunted: move it to this taunter
        } else {
            t.pin = api->RequestEx(a_foe, APMF_API::kIntent_TargetPin, kOwnBasis, &p);
            if (t.pin == APMF_API::kInvalidHandle) {   // refused at the call: the ch.20 seat is not installed
                if (t.entry != APMF_API::kInvalidHandle) api->Release(t.entry);
                s_foes.erase(a_foe);
                if (!s_seatRefused.exchange(true))
                    spdlog::warn("[taunt] Harbinger REFUSED the ch.20 pin on foe {:08X} (target {:08X}) at the call: "
                                 "the ch.20 seat is not installed ([TargetPin] bTargetPin=0 in APMF.ini, VR, a runtime "
                                 "other than 1.6.1170 / 1.5.97, or its address self-check; APMF's log names it). Taunt "
                                 "is INERT for this session. MFO has no direct road.", a_foe, a_taunter);
                return TauntPinResult::Refused;
            }
        }
        t.taunter = a_taunter;
        return repoint ? TauntPinResult::Repointed : TauntPinResult::New;
    }

    TauntPinState TauntPinStateOf(RE::FormID a_foe) {
        const auto* api = TauntApi();
        if (!api) return TauntPinState::None;
        std::scoped_lock lock(s_mx);
        const auto it = s_foes.find(a_foe);
        if (it == s_foes.end() || it->second.pin == APMF_API::kInvalidHandle) return TauntPinState::None;
        return Ended(api, it->second.pin, it->second.pinSeen) ? TauntPinState::Ended : TauntPinState::Standing;
    }

    bool TauntPinEverLive(RE::FormID a_foe) {
        std::scoped_lock lock(s_mx);
        const auto it = s_foes.find(a_foe);
        return it != s_foes.end() && it->second.pinSeen;
    }

    void ReleaseTauntPin(RE::FormID a_foe) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_mx);
        const auto it = s_foes.find(a_foe);
        if (it == s_foes.end()) return;
        ReleaseFoeLocked(api, it->second);
        s_foes.erase(it);
    }

    void ClearTauntClaims() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_mx);
        for (auto& [id, c] : s_idles) ReleaseIdleLocked(api, c);
        for (auto& [id, c] : s_foes) ReleaseFoeLocked(api, c);
        s_idles.clear();
        s_foes.clear();
    }
}
