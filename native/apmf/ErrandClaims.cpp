// apmf/ErrandClaims.cpp -- the Harbinger claims of ONE TOWN ERRAND (ClickUp 86e3t1mx7, round R1a).
// The decisions (when to shop, which vendor, the state machine) are logistics/Errand.cpp's; this file owns
// only the facet claims an errand rides and their lifecycle, in the Deposit.cpp (apmf/Deposit.cpp) shape:
//
//   * ch.19 kIntent_Travel -- walk to the VENDOR (form = the vendor ACTOR reference, fval = the arrival
//                             radius). Its OWN claim, not a loot slot: an errand is not a loot excursion,
//                             and the loot slot table belongs to the loot driver (Deposit.cpp's reasoning).
//                             The v12 leg state is read the way ReadDepositLeg reads a deposit leg.
//   * ch.1  kIntent_MovementBlock -- hold him at the counter while the idles play (Harbinger's recipe
//                             "play an idle at a target": the idle does not hold the NPC still).
//   * ch.12 kIntent_Idle v2 -- the TALK / GIVE / TAKE beats AT the vendor: form = the IDLE record, target =
//                             the vendor. ONE claim per errand: the first beat is RequestEx, every later
//                             beat is a Repoint with the next idle form (a new form replays, ABI >= 17).
//                             The verdict of the CURRENT beat is read with GetIdleState (ABI 24) and is
//                             only believed when the record names that beat's idle (a Repoint is applied
//                             on the game thread, so for a moment the record still describes the last beat).
//
// ABI. Everything here needs ABI >= 24 (GetIdleState; implies idle v2 = 17, the v12 leg state, IsClaimLive):
// an older Harbinger would silently play the v1 idle for a form, so nothing is requested below it
// (ErrandSupported). The legacy road for an older / absent Harbinger is the proximity trade, not this file.
//
// THREADING. Called from the AddTask job worker (the per-follower service tick, the errand Sweep) and from
// the revert with the pump drained. The table is guarded by its OWN mutex, never nested with the bridge's
// g_mx (this file never takes g_mx). APMF's Request / Repoint / Release / leg-state / idle-state reads are
// safe from any thread (APMF_API.h) and never call back into MFO: leaf edges under s_mx.
//
// NOTHING IS SAVED. APMF drops every claim on a load; ClearErrandClaims drops the record of them (a stale
// Release is a harmless no-op).
#include "APMFBridge.h"
#include "APMFBridge_internal.h"   // g_apmf, kOwnBasis
#include "APMF_API.h"
#include "Config.h"                // g_travelGait -- the same walk gait the loot legs use

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kErrandMinAbi = 24;   // GetIdleState (implies idle v2 = 17, the v12 leg state, v6 IsClaimLive)

        // The idle record is created when the claim is filed or re-pointed and published on the game thread
        // at the next Drain; this wall-clock grace only covers the instant before it is visible. A state
        // still Unknown (or still describing the PREVIOUS beat) after it is a failed beat. It judges nothing
        // about whether an idle played: Harbinger judges that (kIdleConfirmWindowMs).
        constexpr auto kIdleRecordGrace = std::chrono::milliseconds(2500);

        struct ErrandClaims {
            APMF_API::Handle travel = APMF_API::kInvalidHandle;
            APMF_API::Handle hold   = APMF_API::kInvalidHandle;
            APMF_API::Handle idle   = APMF_API::kInvalidHandle;
            RE::FormID       dest   = 0;       // the vendor reference the leg walks to
            // R5: the travel claim's liveness (the ClaimDead rule, as for the hold / idle): RepointErrandTravel must not "move" a claim
            // Harbinger already ended (Repoint is void and would silently do nothing).
            bool             travelEverLive = false;
            std::chrono::steady_clock::time_point travelFiled{};
            // The leg-state seq read just BEFORE our RequestEx: a state still carrying it is the previous
            // leg's end, never ours (the loot M2 rule, apmf/Excursion.cpp NoteStaleSeqLocked).
            std::uint32_t    staleSeq = 0;
            bool             hasStale = false;
            RE::FormID       beatIdle = 0;     // the idle the CURRENT beat plays (what the record must name)
            std::chrono::steady_clock::time_point beatFiled{};
            // Harbinger RELEASES a ch.12 claim itself when the play fails (NotConfirmed / NoTransition / Refused /
            // NotPlayed) and a Repoint on a released handle does nothing, so a handle is judged dead only after it
            // was seen live once, or after the publish grace (a just-filed claim is not in IsClaimLive's map until
            // the next Drain; the Deposit.cpp rule).
            bool idleEverLive = false, holdEverLive = false;
            std::chrono::steady_clock::time_point holdFiled{};
            std::chrono::steady_clock::time_point idleFiled{};   // set ONLY at the idle's RequestEx (beatFiled moves on every Repoint)
        };
        std::mutex                                     s_mx;
        std::unordered_map<RE::FormID, ErrandClaims>   s_claims;   // keyed by follower; guarded by s_mx

        const APMF_API::APMF_API_v24* ErrandApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kErrandMinAbi) return nullptr;
            auto* v24 = reinterpret_cast<const APMF_API::APMF_API_v24*>(api);
            return (v24->GetTravelLegState && v24->GetIdleState) ? v24 : nullptr;
        }

        constexpr auto kPublishGrace = std::chrono::seconds(3);
        // s_mx HELD. True = the handle is a claim Harbinger no longer holds.
        bool ClaimDead(const APMF_API::APMF_API_v24* a_api, APMF_API::Handle a_h, bool& a_everLive,
                       std::chrono::steady_clock::time_point a_filed) {
            if (a_h == APMF_API::kInvalidHandle) return false;
            if (a_api->IsClaimLive(a_h)) { a_everLive = true; return false; }
            return a_everLive || std::chrono::steady_clock::now() - a_filed > kPublishGrace;
        }

        // s_mx HELD. The ch.1 hold (kIntent_MovementBlock, no param), filed once and re-filed when Harbinger ended it. A refusal is logged
        // (a_without names what then runs without the stand-still). True = a hold handle stands.
        bool FileHoldLocked(const APMF_API::APMF_API_v24* a_api, RE::FormID a_follower, ErrandClaims& c, const char* a_without) {
            if (ClaimDead(a_api, c.hold, c.holdEverLive, c.holdFiled)) {
                spdlog::info("[errand] {:08X}: the ch.1 hold ended (Harbinger released it) -- filing a fresh one", a_follower);
                c.hold = APMF_API::kInvalidHandle;
            }
            if (c.hold == APMF_API::kInvalidHandle) {
                APMF_API::APMF_Param hp{};
                c.holdEverLive = false;
                c.holdFiled = std::chrono::steady_clock::now();
                c.hold = a_api->RequestEx(a_follower, APMF_API::kIntent_MovementBlock, kOwnBasis, &hp);
                if (c.hold == APMF_API::kInvalidHandle)
                    spdlog::warn("[errand] {:08X}: ch.1 hold REFUSED by Harbinger -- {} (APMF.log names the reason)", a_follower, a_without);
            }
            return c.hold != APMF_API::kInvalidHandle;
        }

        // mfo-272: the ch.19 kTravel_CrossCells flag (marth 2026-10-25 "Moveto using harbinger will have a flag for cell crossing"),
        // shipped by Harbinger in ABI v30: a ref leg paths load doors natively, arrives by cell + radius and holds.
        constexpr std::uint32_t kCrossCellsAbi = 30;

        std::uint32_t TravelFlags(const APMF_API::APMF_API_v24* a_api) {
            const int gait = std::clamp(Config::g_travelGait.load(), 0, 3);
            std::uint32_t f = APMF_API::kTravel_SpeedSet | ((static_cast<std::uint32_t>(gait) << 3) & APMF_API::kTravel_SpeedMask);
            if constexpr (kCrossCellsAbi != 0) { if (a_api->abiVersion >= kCrossCellsAbi) f |= APMF_API::kTravel_CrossCells; }
            return f;
        }

        void ReleaseLocked(const APMF_API::APMF_API_v2* a_api, ErrandClaims& a_c) {
            // Idle first (its guarded reset runs only if the idle is still HELD), then the hold, then the walk.
            for (auto* h : { &a_c.idle, &a_c.hold, &a_c.travel }) {
                if (a_api && *h != APMF_API::kInvalidHandle) a_api->Release(*h);
                *h = APMF_API::kInvalidHandle;
            }
        }

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
        const char* IdleEndName(std::uint32_t e) {
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

    bool ErrandSupported() { return ErrandApi() != nullptr; }

    std::uint32_t ErrandApiVersion() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        return api ? api->abiVersion : 0;
    }

    bool ClaimErrandTravel(RE::FormID a_follower, RE::FormID a_dest, float a_radius) {
        auto* api = ErrandApi();
        if (!api || a_follower == 0 || a_dest == 0) return false;
        std::scoped_lock lock(s_mx);
        auto& c = s_claims[a_follower];
        if (c.travel != APMF_API::kInvalidHandle) return c.dest == a_dest;   // one errand = one destination here

        APMF_API::APMF_TravelLegInfo pre{};
        pre.size = static_cast<std::uint32_t>(sizeof(APMF_API::APMF_TravelLegInfo));
        api->GetTravelLegState(a_follower, &pre);
        c.hasStale = true;
        c.staleSeq = pre.seq;

        APMF_API::APMF_Param p{};
        p.form = a_dest;           // the vendor reference (loaded -- Errand.cpp checked)
        p.fval = a_radius;         // APMF clamps to [50,512]
        const int gait = std::clamp(Config::g_travelGait.load(), 0, 3);
        p.ival = static_cast<std::int32_t>(TravelFlags(api));   // the gait, plus kTravel_CrossCells once Harbinger carries it (mfo-272)
        const APMF_API::Handle h = api->RequestEx(a_follower, APMF_API::kIntent_Travel, kOwnBasis, &p);
        if (h == APMF_API::kInvalidHandle) {
            spdlog::warn("[errand] {:08X}: the walk to {:08X} was REFUSED by Harbinger (radius {:.0f}) -- APMF.log "
                         "names the reason (esl absent, [Travel] bTravel=0, VR)", a_follower, a_dest, a_radius);
            s_claims.erase(a_follower);
            return false;
        }
        c.travel = h;
        c.dest   = a_dest;
        c.travelEverLive = false;
        c.travelFiled = std::chrono::steady_clock::now();
        spdlog::info("[errand] {:08X}: walk CLAIMED (ch.19) -> {:08X} radius {:.0f} gait {} handle {}",
                     a_follower, a_dest, a_radius, gait, h);
        return true;
    }

    bool RepointErrandTravel(RE::FormID a_follower, RE::FormID a_dest, float a_radius) {
        auto* api = ErrandApi();
        if (!api || a_follower == 0 || a_dest == 0) return false;
        std::scoped_lock lock(s_mx);
        const auto it = s_claims.find(a_follower);
        if (it == s_claims.end() || it->second.travel == APMF_API::kInvalidHandle) return false;
        auto& c = it->second;
        // R5 (review SEV-4): APMF's Repoint returns nothing, so a claim Harbinger already ended would be "re-pointed" into the void and the
        // follower would stand still. A dead claim is forgotten here and FALSE returned: the caller files a fresh ClaimErrandTravel.
        if (ClaimDead(api, c.travel, c.travelEverLive, c.travelFiled)) {
            spdlog::info("[errand] {:08X}: the ch.19 walk claim (handle {}) is no longer live -- not re-pointing it (a fresh claim follows)",
                         a_follower, c.travel);
            c.travel = APMF_API::kInvalidHandle;
            return false;
        }
        APMF_API::APMF_TravelLegInfo pre{};
        pre.size = static_cast<std::uint32_t>(sizeof(APMF_API::APMF_TravelLegInfo));
        api->GetTravelLegState(a_follower, &pre);
        c.hasStale = true;
        c.staleSeq = pre.seq;   // the state standing now is the OLD leg's end, never the new one's
        APMF_API::APMF_Param p{};
        p.form = a_dest;
        p.fval = a_radius;
        p.ival = static_cast<std::int32_t>(TravelFlags(api));   // the gait, plus kTravel_CrossCells once Harbinger carries it (mfo-272)
        api->Repoint(c.travel, &p);
        c.dest = a_dest;
        spdlog::info("[errand] {:08X}: walk RE-POINTED (ch.19) -> {:08X} radius {:.0f} handle {}", a_follower, a_dest, a_radius, c.travel);
        return true;
    }

    bool ReadErrandLeg(RE::FormID a_follower, LootLegState& a_out) {
        a_out = LootLegState{};
        auto* api = ErrandApi();
        if (!api) return false;
        ErrandClaims c;
        {
            std::scoped_lock lock(s_mx);
            auto it = s_claims.find(a_follower);
            if (it == s_claims.end() || it->second.travel == APMF_API::kInvalidHandle) return false;
            c = it->second;
        }
        APMF_API::APMF_TravelLegInfo info{};
        info.size = static_cast<std::uint32_t>(sizeof(APMF_API::APMF_TravelLegInfo));
        api->GetTravelLegState(a_follower, &info);
        a_out.state       = info.state;
        a_out.seq         = info.seq;
        a_out.msInState   = info.msInState;
        a_out.blockedMs   = info.blockedMs;
        a_out.speed       = info.speed;
        a_out.blockerKind = info.blockerKind;
        a_out.blocker     = info.blocker;
        a_out.dest        = info.destForm;
        a_out.stall       = RE::NiPoint3{ info.stallX, info.stallY, info.stallZ };
        a_out.ours = info.destForm == c.dest &&
                     (info.ownerHandle == c.travel || info.ownerHandle == APMF_API::kInvalidHandle) &&
                     !(c.hasStale && info.seq == c.staleSeq);
        return true;
    }

    bool PlayErrandIdle(RE::FormID a_follower, RE::FormID a_idle, RE::FormID a_target) {
        auto* api = ErrandApi();
        if (!api || a_follower == 0 || a_idle == 0 || a_target == 0 || a_target == a_follower) return false;
        std::scoped_lock lock(s_mx);
        auto& c = s_claims[a_follower];

        // HOLD STILL for the beats (ch.1, no param), once. A refusal is logged and the idle still plays: the
        // hold is what keeps the follow package from walking him off the counter mid-animation, not what plays it.
        FileHoldLocked(api, a_follower, c, "the idles play without a stand-still");

        APMF_API::APMF_Param p{};
        p.form   = a_idle;     // the IDLE record (an older APMF would ignore it -- gated above)
        p.target = a_target;   // the vendor: the engine evaluates the idle's conditions against it
        // A previous beat's claim that Harbinger released (the failed talk idle is the expected case outside a
        // conversation) cannot be re-pointed: invalidate it and file a FRESH claim for this beat.
        // Harbinger's OWN statement that the claim ended: GetIdleState's endCause (readable for kIdleStateGraceMs, 10 s). A
        // refused talk idle releases its claim at the same drain that publishes it, so IsClaimLive never reads true.
        bool idleEnded = false;
        if (c.idle != APMF_API::kInvalidHandle) {
            APMF_API::APMF_IdleStateInfo ii{};
            ii.size = sizeof(ii);
            api->GetIdleState(c.idle, &ii);
            idleEnded = ii.endCause != APMF_API::kIdleEnd_None;
        }
        if (idleEnded || ClaimDead(api, c.idle, c.idleEverLive, c.idleFiled)) {
            spdlog::info("[errand] {:08X}: the previous idle claim (handle {}) was released by Harbinger -- filing a "
                         "fresh ch.12 claim for this beat", a_follower, c.idle);
            c.idle = APMF_API::kInvalidHandle;
        }
        if (c.idle == APMF_API::kInvalidHandle) {
            c.idleEverLive = false;
            c.idleFiled = std::chrono::steady_clock::now();
            c.idle = api->RequestEx(a_follower, APMF_API::kIntent_Idle, kOwnBasis, &p);
            if (c.idle == APMF_API::kInvalidHandle) {
                spdlog::warn("[errand] {:08X}: ch.12 idle v2 REFUSED by Harbinger (idle {:08X} at {:08X}) -- APMF.log "
                             "'[apmf][idle] idle v2 claim refused' names why ([Idle] bIdleV2=0, runtime, self-check)",
                             a_follower, a_idle, a_target);
                return false;
            }
        } else {
            api->Repoint(c.idle, &p);   // the next beat: a new form replays
        }
        c.beatIdle  = a_idle;
        c.beatFiled = std::chrono::steady_clock::now();
        spdlog::info("[errand] {:08X}: idle beat REQUESTED (ch.12 v2) idle {:08X} at {:08X} handle {} (hold handle {})",
                     a_follower, a_idle, a_target, c.idle, c.hold);
        return true;
    }

    ErrandIdleReading ReadErrandIdle(RE::FormID a_follower) {
        ErrandIdleReading out;
        auto* api = ErrandApi();
        if (!api) return out;
        std::scoped_lock lock(s_mx);
        const auto it = s_claims.find(a_follower);
        if (it == s_claims.end() || it->second.idle == APMF_API::kInvalidHandle) return out;
        auto& c = it->second;
        APMF_API::APMF_IdleStateInfo info{};
        info.size = sizeof(info);
        if (api->IsClaimLive(c.idle)) c.idleEverLive = true;
        const std::uint32_t st = api->GetIdleState(c.idle, &info);
        out.detail = std::string("state=") + IdleStateName(st) + " endCause=" + IdleEndName(info.endCause);
        if (info.unconfirmableWhy) out.detail += " unconfirmableWhy=" + std::to_string(info.unconfirmableWhy);
        using V = ErrandIdleVerdict;
        const bool inGrace = std::chrono::steady_clock::now() - c.beatFiled < kIdleRecordGrace;
        // A record that still names the PREVIOUS beat's idle is not about this beat yet (the Repoint is applied
        // on the game thread): wait inside the grace, a failed beat past it.
        if (info.idle != 0 && info.idle != c.beatIdle) {
            out.verdict = inGrace ? V::Waiting : V::Failed;
            out.detail += std::format(" (record still names idle {:08X}, this beat is {:08X})", info.idle, c.beatIdle);
            return out;
        }
        if (info.endCause == APMF_API::kIdleEnd_Superseded) {
            out.detail += " (another claim owns ch.12 on him)";
            out.verdict = V::Failed;
        } else if (st == APMF_API::kIdleState_Confirmed || st == APMF_API::kIdleState_ConfirmedWeak) {
            out.verdict = V::Played;
            out.weak = st == APMF_API::kIdleState_ConfirmedWeak;
        } else if (info.endCause != APMF_API::kIdleEnd_None) {
            out.verdict = V::Failed;   // ended without playing (NotConfirmed, NoTransition, Refused, NotPlayed, Released, Gone, ActorDied)
        } else if (st == APMF_API::kIdleState_Unknown) {
            out.verdict = inGrace ? V::Waiting : V::Failed;
        } else if (st == APMF_API::kIdleState_Unconfirmable || st == APMF_API::kIdleState_Carried) {
            out.verdict = V::Failed;   // live, but no evidence for this play: never act on it as played
            if (st == APMF_API::kIdleState_Unconfirmable) out.unconfirmableWhy = info.unconfirmableWhy;   // (a caller may know this why is benign)
        } else {
            out.verdict = V::Waiting;  // Pending or Accepted: Harbinger judges the window, MFO does not time it
        }
        return out;
    }

    bool ErrandCrossCellsSupported() {
        auto* api = ErrandApi();
        if constexpr (kCrossCellsAbi == 0) { (void)api; return false; } else { return api && api->abiVersion >= kCrossCellsAbi; }
    }

    bool HoldErrand(RE::FormID a_follower) {
        auto* api = ErrandApi();
        if (!api || a_follower == 0) return false;
        std::scoped_lock lock(s_mx);
        auto& c = s_claims[a_follower];
        const bool had = c.hold != APMF_API::kInvalidHandle;
        const bool held = FileHoldLocked(api, a_follower, c, "he is not held at the stop while it waits");
        if (held && !had) spdlog::info("[errand] {:08X}: ch.1 hold FILED (handle {}) -- he stands at the stop for the whole wait", a_follower, c.hold);
        return held;
    }

    void ReleaseErrandIdle(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_mx);
        auto it = s_claims.find(a_follower);
        if (it == s_claims.end()) return;
        auto& c = it->second;
        for (auto* h : { &c.idle, &c.hold }) {
            if (api && *h != APMF_API::kInvalidHandle) api->Release(*h);
            *h = APMF_API::kInvalidHandle;
        }
        c.beatIdle = 0;
        c.idleEverLive = c.holdEverLive = false;
    }

    void ReleaseErrandTravel(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_mx);
        auto it = s_claims.find(a_follower);
        if (it == s_claims.end() || it->second.travel == APMF_API::kInvalidHandle) return;
        auto& c = it->second;
        if (api) api->Release(c.travel);
        spdlog::info("[errand] {:08X}: the ch.19 walk claim (handle {}, dest {:08X}) RELEASED -- the work claim holds him now", a_follower, c.travel, c.dest);
        c.travel = APMF_API::kInvalidHandle;
        c.dest = 0;
        c.travelEverLive = false;
    }

    void ReleaseErrand(RE::FormID a_follower) {
        ReleaseErrandWork(a_follower);   // R7: the errand's ch.9 WORK claim (apmf/ErrandWork.cpp, its own mutex) goes with the rest
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_mx);
        auto it = s_claims.find(a_follower);
        if (it == s_claims.end()) return;
        ReleaseLocked(api, it->second);
        s_claims.erase(it);
    }

    void ClearErrandClaims() {
        ClearErrandWork();   // R7
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_mx);
        for (auto& [id, c] : s_claims) ReleaseLocked(api, c);
        s_claims.clear();
    }
}
