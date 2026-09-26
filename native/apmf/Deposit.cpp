// apmf/Deposit.cpp -- the Harbinger claims of ONE LOTD museum deposit trip
// (feat/mfo-lotd, ClickUp 86e3edghj round L3). The trip itself (when to go, which
// crate, what to ship, the transfer) is logistics/Lotd.cpp's; this file only owns
// the three facet claims the trip rides and their lifecycle:
//
//   * ch.19 kIntent_Travel  -- walk to the crate (form = the crate REFERENCE, fval =
//                              the arrival radius). Its OWN claim, not a loot slot:
//                              a deposit is not a loot excursion, and the loot slot
//                              table (keyed by Packages::kMaxLootSlots) belongs to
//                              the loot driver. The v12 leg state is read exactly
//                              the way ReadLootTravelLeg reads a loot leg.
//   * ch.1  kIntent_MovementBlock -- hold him at the crate for the length of the
//                              idle. Harbinger's own recipe ("play an idle at a
//                              target", Docs/INTEGRATION.md v17): the idle does NOT
//                              hold the NPC still, a follow package walks him off.
//   * ch.12 kIntent_Idle v2 -- play IdleGive (Skyrim.esm 0x0B5E20) AT the crate:
//                              form = the IDLE record, target = the crate ref.
//
// ABI. ch.12 v2 is ABI v17 and an OLDER Harbinger does NOT refuse a form -- it
// silently plays the v1 IdleForceDefaultState instead -- so nothing here is ever
// requested below v17 (DepositSupported). v17 implies the v12 leg-state slot and
// the v6 IsClaimLive slot, both used below.
//
// THREADING. Every entry point is called from the AddTask job worker (the
// per-follower service tick) or, for ClearDepositClaims, from the revert with the
// pump drained. The table is guarded by its OWN mutex, never nested with the
// bridge's g_mx (this file never takes g_mx). APMF's Request/Repoint/Release and
// its leg-state / liveness reads are safe from any thread (APMF_API.h) and never
// call back into MFO, so calling them under s_mx is a leaf edge.
//
// NOTHING IS SAVED. APMF drops every claim on a load; ClearDepositClaims drops the
// record of them (a stale Release is a harmless no-op).
#include "APMFBridge.h"
#include "APMFBridge_internal.h"   // g_apmf, kOwnBasis
#include "APMF_API.h"
#include "Config.h"                // g_travelGait -- the same walk gait the loot legs use

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kDepositMinAbi = 17;   // ch.12 Idle v2 (form + target)

        // A just-filed claim is not in APMF's PUBLISHED map until its next writer
        // Drain, and IsClaimLive reads only that map. So an idle claim is judged
        // ENDED only after it was seen live once, or after this long unseen. A
        // FLOOR, not an expiry (principle 9): it only delays noticing an end.
        constexpr auto kIdleNeverLiveGrace = std::chrono::seconds(3);

        struct DepositClaims {
            APMF_API::Handle travel = APMF_API::kInvalidHandle;
            APMF_API::Handle hold   = APMF_API::kInvalidHandle;
            APMF_API::Handle idle   = APMF_API::kInvalidHandle;
            RE::FormID       crate  = 0;
            // The leg-state seq read just BEFORE our RequestEx: a state still
            // carrying it is the previous leg's end, never ours (the loot M2 rule,
            // apmf/Excursion.cpp NoteStaleSeqLocked).
            std::uint32_t    staleSeq = 0;
            bool             hasStale = false;
            std::chrono::steady_clock::time_point idleFiled{};
            bool             idleEverLive = false;
        };
        std::mutex                                     s_mx;
        std::unordered_map<RE::FormID, DepositClaims>  s_claims;   // guarded by s_mx

        const APMF_API::APMF_API_v12* DepositApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kDepositMinAbi) return nullptr;
            auto* v12 = reinterpret_cast<const APMF_API::APMF_API_v12*>(api);
            return v12->GetTravelLegState ? v12 : nullptr;
        }

        bool ReadInfo(const APMF_API::APMF_API_v12* a_api, RE::FormID a_actor,
                      APMF_API::APMF_TravelLegInfo& a_info) {
            static_assert(sizeof(APMF_API::APMF_TravelLegInfo) >= APMF_API::kTravelLegInfoV12Size,
                          "the deposit reads the v12 leg-state prefix");
            a_info      = APMF_API::APMF_TravelLegInfo{};
            a_info.size = static_cast<std::uint32_t>(sizeof(APMF_API::APMF_TravelLegInfo));
            a_api->GetTravelLegState(a_actor, &a_info);
            return true;
        }

        void ReleaseLocked(const APMF_API::APMF_API_v2* a_api, DepositClaims& a_c) {
            // Idle first (its guarded reset runs only if the idle is still HELD),
            // then the hold, then the walk.
            for (auto* h : { &a_c.idle, &a_c.hold, &a_c.travel }) {
                if (a_api && *h != APMF_API::kInvalidHandle) a_api->Release(*h);
                *h = APMF_API::kInvalidHandle;
            }
        }
    }

    bool DepositSupported() { return DepositApi() != nullptr; }

    std::uint32_t DepositApiVersion() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        return api ? api->abiVersion : 0;
    }

    bool ClaimDepositTravel(RE::FormID a_follower, RE::FormID a_crate, float a_radius) {
        auto* api = DepositApi();
        if (!api || a_follower == 0 || a_crate == 0) return false;
        std::scoped_lock lock(s_mx);
        auto& c = s_claims[a_follower];
        if (c.travel != APMF_API::kInvalidHandle) {
            // One trip = one destination: Lotd.cpp never re-points a live trip.
            return c.crate == a_crate;
        }
        APMF_API::APMF_TravelLegInfo pre{};
        c.hasStale = ReadInfo(api, a_follower, pre);
        c.staleSeq = c.hasStale ? pre.seq : 0;

        APMF_API::APMF_Param p{};
        p.form = a_crate;          // the crate REFERENCE (loaded -- Lotd.cpp checked)
        p.fval = a_radius;         // APMF clamps to [50,512]
        const int gait = std::clamp(Config::g_travelGait.load(), 0, 3);   // v12+ (v17 here)
        p.ival = static_cast<std::int32_t>(APMF_API::kTravel_ReleaseOnTargetDead | APMF_API::kTravel_SpeedSet |
                                           ((static_cast<std::uint32_t>(gait) << 3) & APMF_API::kTravel_SpeedMask));
        const APMF_API::Handle h = api->RequestEx(a_follower, APMF_API::kIntent_Travel, kOwnBasis, &p);
        if (h == APMF_API::kInvalidHandle) {
            spdlog::warn("[lotd] {:08X}: deposit walk REFUSED by Harbinger (crate {:08X}, radius {:.0f}) -- "
                         "APMF.log names the reason (esl absent, [Travel] bTravel=0, VR, or all 8 travel "
                         "slots busy)", a_follower, a_crate, a_radius);
            s_claims.erase(a_follower);
            return false;
        }
        c.travel = h;
        c.crate  = a_crate;
        spdlog::info("[lotd] {:08X}: deposit walk CLAIMED (ch.19) -> crate {:08X} radius {:.0f} gait {} handle {}",
                     a_follower, a_crate, a_radius, gait, h);
        return true;
    }

    bool ReadDepositLeg(RE::FormID a_follower, LootLegState& a_out) {
        a_out = LootLegState{};
        auto* api = DepositApi();
        if (!api) return false;
        DepositClaims c;
        {
            std::scoped_lock lock(s_mx);
            auto it = s_claims.find(a_follower);
            if (it == s_claims.end() || it->second.travel == APMF_API::kInvalidHandle) return false;
            c = it->second;
        }
        APMF_API::APMF_TravelLegInfo info{};
        ReadInfo(api, a_follower, info);
        a_out.state       = info.state;
        a_out.seq         = info.seq;
        a_out.msInState   = info.msInState;
        a_out.blockedMs   = info.blockedMs;
        a_out.speed       = info.speed;
        a_out.blockerKind = info.blockerKind;
        a_out.blocker     = info.blocker;
        a_out.dest        = info.destForm;
        a_out.stall       = RE::NiPoint3{ info.stallX, info.stallY, info.stallZ };
        a_out.ours = info.destForm == c.crate &&
                     (info.ownerHandle == c.travel || info.ownerHandle == APMF_API::kInvalidHandle) &&
                     !(c.hasStale && info.seq == c.staleSeq);
        return true;
    }

    bool ClaimDepositIdle(RE::FormID a_follower, RE::FormID a_idle, RE::FormID a_target) {
        auto* api = DepositApi();
        if (!api || a_follower == 0 || a_idle == 0 || a_target == 0) return false;
        std::scoped_lock lock(s_mx);
        auto& c = s_claims[a_follower];
        if (c.idle != APMF_API::kInvalidHandle) return true;   // already playing this trip's idle

        // HOLD STILL for the idle (ch.1, no param). A refusal is logged and the idle
        // still plays: the hold is what keeps the follow package from walking him
        // off the crate mid-animation, not what plays it.
        if (c.hold == APMF_API::kInvalidHandle) {
            APMF_API::APMF_Param hp{};
            c.hold = api->RequestEx(a_follower, APMF_API::kIntent_MovementBlock, kOwnBasis, &hp);
            if (c.hold == APMF_API::kInvalidHandle)
                spdlog::warn("[lotd] {:08X}: ch.1 hold REFUSED by Harbinger -- the give idle plays without a "
                             "stand-still (APMF.log names the reason)", a_follower);
        }

        APMF_API::APMF_Param p{};
        p.form   = a_idle;     // the IDLE record (v17: an older APMF would ignore it -- gated above)
        p.target = a_target;   // the crate: the engine evaluates the idle's conditions against it
        c.idle = api->RequestEx(a_follower, APMF_API::kIntent_Idle, kOwnBasis, &p);
        if (c.idle == APMF_API::kInvalidHandle) {
            spdlog::warn("[lotd] {:08X}: ch.12 idle v2 REFUSED by Harbinger (idle {:08X} at {:08X}) -- APMF.log "
                         "'[apmf][idle] idle v2 claim refused' names why ([Idle] bIdleV2=0, runtime, "
                         "self-check)", a_follower, a_idle, a_target);
            return false;
        }
        c.idleFiled    = std::chrono::steady_clock::now();
        c.idleEverLive = false;
        spdlog::info("[lotd] {:08X}: give idle CLAIMED (ch.12 v2) idle {:08X} at crate {:08X} handle {} "
                     "(hold handle {})", a_follower, a_idle, a_target, c.idle, c.hold);
        return true;
    }

    int DepositIdleStatus(RE::FormID a_follower) {
        auto* api = DepositApi();
        if (!api) return 0;
        std::scoped_lock lock(s_mx);
        auto it = s_claims.find(a_follower);
        if (it == s_claims.end() || it->second.idle == APMF_API::kInvalidHandle) return 0;
        auto& c = it->second;
        if (api->IsClaimLive(c.idle)) {
            c.idleEverLive = true;
            return 1;
        }
        if (!c.idleEverLive && std::chrono::steady_clock::now() - c.idleFiled < kIdleNeverLiveGrace)
            return 0;   // not published yet
        return 2;       // Harbinger ended it (refused / unplayable / engine refused): APMF.log says why
    }

    void ReleaseDeposit(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_mx);
        auto it = s_claims.find(a_follower);
        if (it == s_claims.end()) return;
        ReleaseLocked(api, it->second);
        s_claims.erase(it);
    }

    void ClearDepositClaims() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_mx);
        for (auto& [id, c] : s_claims) ReleaseLocked(api, c);
        s_claims.clear();
    }
}
