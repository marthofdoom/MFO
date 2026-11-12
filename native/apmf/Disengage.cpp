// apmf/Disengage.cpp -- THE DISENGAGE DRIVE (class plan card H3): run to a point mid-fight and stay there.
// Two Harbinger claims, filed and released together, each its own facet (Harbinger's intents stay orthogonal):
//   * ch.24 CombatApproach on a POINT (ABI v39, kApproach_Point | kApproach_Hold): the follower's combat area is the
//     small disc (point, R). He is outside it, so the engine's own Return To Combat Area takes the Movement half and
//     paths him there at its RUN speed (class-m0-re.md section 1: speed enum 3 on all three runtimes). kApproach_Hold
//     keeps the bound after he arrives, so he stays inside R until MFO releases it.
//   * ch.7 CombatAction deny (optional, a_deny; APMFBridge.h DisengageDeny): the HIDE RUN denies Offense | Cast (the
//     attack, bash and every cast leaf: he does not stop to swing or cast on the way and does not strike from the
//     point). The FLEE denies Offense alone, which from Harbinger ABI v40 means HOSTILE actions only (marth 2026-11-11):
//     attacks and hostile casts are denied, his own heals land. Below ABI 40 Offense would deny his heals too, so the
//     flee files NO ch.7 claim there (logged once). Without a deny he fights the whole way (the give-ground step,
//     apmf/GroundApproach.cpp).
// NOT CLAIMED: facing. Whether the engine runs him facing forward or keeps him facing his target (a strafe or a
// backpedal) is NOT known (class-m0-re.md: "Facing: UNPROVEN"). Harbinger's passive gait probe answers it in the field
// ([ch.24] GAIT PROBE lines, [CombatApproach] bApproachGaitProbe). The drive works either way: arrival is the engine's own
// "inside the area" answer, never a pose. A facing seat is NOT built (marth's default for B#16: only if the field shows a
// backpedal).
// MFO DECLARES the point; Harbinger does not test it. A point the engine cannot path to ends in Harbinger as
// EngineDropped (3 s, a warning there) and reads Failed here.
// PRECEDENCE (one ch.24 claim per follower, every MFO client files at kOwnBasis, the EARLIER claim keeps the channel):
// a held-heal approach outranks a disengage (never filed while it stands). The offense approach and the give-ground
// step are released once Harbinger has ACCEPTED this request (never before), so this claim owns. A claim that still never owns reads Failed after 700 ms.
// Also here: FindStandPoint, the one wrapper of Harbinger's FindEmptySpace space query (ABI v11, MAIN THREAD ONLY).
// Threads: Request / Service / Release on the pump's AddTask worker; own mutex g_disengageMx, a leaf: nothing is called
// under it. The one nesting is APMFBridge::ClearTransientState (g_mx -> g_disengageMx, as its siblings); this file never
// takes g_mx. Nothing is saved.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"

#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kDisengageMinAbi = 39;   // the ABI that first reads kApproach_Point
        constexpr std::uint32_t kSpaceMinAbi     = 11;   // FindEmptySpace
        constexpr std::uint32_t kHostileOffenseMinAbi = 40;   // ch.7: Offense means hostile (a beneficial cast passes)
        std::atomic<bool> g_oldOffenseLogged{ false };

        struct Drive {
            APMF_API::Handle approach = APMF_API::kInvalidHandle;   // ch.24, the point
            APMF_API::Handle deny     = APMF_API::kInvalidHandle;   // ch.7, Offense (kInvalidHandle = not asked / refused)
            RE::NiPoint3     point{};
            float            radius = 0.0f;
            bool             arrived = false;
            std::chrono::steady_clock::time_point at{};
        };
        std::mutex g_disengageMx;
        std::unordered_map<RE::FormID, Drive> g_drives;   // follower -> his standing drive

        const APMF_API::APMF_API_v19* DisengageApi() {
            const auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kDisengageMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v19*>(api);
        }

        const char* EndName(std::uint32_t a_state) {
            switch (a_state) {
            case APMF_API::kApproachState_Arrived:       return "Harbinger ended it as arrived (the hold bit was not kept)";
            case APMF_API::kApproachState_TargetLost:    return "he left the point's cell space";
            case APMF_API::kApproachState_CombatEnded:   return "combat ended";
            case APMF_API::kApproachState_EngineDropped: return "the engine dropped the bound (no path to the point)";
            case APMF_API::kApproachState_ActorGone:     return "actor gone";
            case APMF_API::kApproachState_Released:      return "released";
            case APMF_API::kApproachState_Refused:       return "refused";
            default:                                     return "ended";
            }
        }

        void ReleaseBoth(const Drive& a_d) {
            const auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api) return;
            if (a_d.approach != APMF_API::kInvalidHandle) api->Release(a_d.approach);   // a no-op on a claim Harbinger ended
            if (a_d.deny != APMF_API::kInvalidHandle) api->Release(a_d.deny);
        }
    }

    bool DisengageSupported() { return DisengageApi() != nullptr; }

    bool DisengageStands(RE::FormID a_follower) {
        std::scoped_lock lk(g_disengageMx);
        return g_drives.contains(a_follower);
    }

    const char* RequestDisengage(RE::Actor* a_follower, const RE::NiPoint3& a_point, float a_radius, DisengageDeny a_deny) {
        const auto* api = DisengageApi();
        if (!a_follower) return "no follower";
        if (!api) return "no disengage road (Harbinger absent or older than ABI 39)";
        if (!std::isfinite(a_point.x) || !std::isfinite(a_point.y) || !std::isfinite(a_point.z) || !(a_radius > 0.0f))
            return "the declared point or radius is not a finite number";
        const RE::FormID id = a_follower->GetFormID();
        if (HealApproachStands(id)) return "a held heal's walk to its recipient stands (it outranks a disengage)";
        {
            std::scoped_lock lk(g_disengageMx);
            if (g_drives.contains(id)) return "a disengage already stands";
        }
        APMF_API::APMF_Param prm{};
        prm.ival = static_cast<std::int32_t>(APMF_API::kApproach_Point | APMF_API::kApproach_Hold);   // target stays 0
        prm.fval = a_radius;
        prm.posX = a_point.x;
        prm.posY = a_point.y;
        prm.posZ = a_point.z;
        Drive d;
        d.approach = api->RequestEx(id, APMF_API::kIntent_CombatApproach, kOwnBasis, &prm);
        if (d.approach == APMF_API::kInvalidHandle) return "Harbinger refused the point approach at the call (its log says why)";
        // ACCEPTED. Only now does anything of his change: one ch.24 claim per follower, ours must be the standing one, so the
        // offense approach and the give-ground step end here (a refused request above leaves them exactly as they were, which
        // is what keeps cast/Fire.cpp's "transparent" answer true).
        ReleaseOffenseApproach(id, "disengaging");
        ReleaseGroundStep(id, "disengaging");
        ReleaseRangeClose(id, "disengaging");   // H4: the drive owns the one ch.24 claim now
        // The ch.7 deny of the leg. `said` is what the start line reports.
        std::string said = "not asked";
        std::uint32_t cats = 0;
        if (a_deny == DisengageDeny::OffenseAndCast) {
            cats = APMF_API::kCombatActionCat_Offense | APMF_API::kCombatActionCat_Cast;
        } else if (a_deny == DisengageDeny::HostileOffense) {
            if (api->abiVersion >= kHostileOffenseMinAbi) {
                cats = APMF_API::kCombatActionCat_Offense;
            } else {
                // An older Harbinger denies EVERY cast under Offense, his heals included: not the deny this leg declares.
                // No claim at all, said once; never a mask that does something else than asked (principle 7).
                said = fmt::format("NOT FILED (Harbinger ABI {} < {})", api->abiVersion, kHostileOffenseMinAbi);
                if (!g_oldOffenseLogged.exchange(true))
                    spdlog::warn("[disengage] Harbinger ABI {} is older than {}: its ch.7 Offense deny also denies beneficial "
                                 "casts, which would stop a fleeing follower's own heals. A flee files NO ch.7 deny on this "
                                 "Harbinger: he runs to the point and may still swing or cast on the way (MFO itself names "
                                 "no offense during a flee). Update Harbinger for the hostile-only deny. Said once.",
                                 api->abiVersion, kHostileOffenseMinAbi);
            }
        }
        if (cats != 0) {
            const char* what = a_deny == DisengageDeny::OffenseAndCast ? "Offense|Cast (attacks and every cast)"
                                                                       : "Offense (attacks and hostile casts; his heals pass)";
            APMF_API::APMF_Param deny{};
            deny.ival = static_cast<std::int32_t>(cats);
            d.deny = api->RequestEx(id, APMF_API::kIntent_CombatAction, kOwnBasis, &deny);
            if (d.deny == APMF_API::kInvalidHandle) {
                said = fmt::format("{} REFUSED", what);
                spdlog::warn("[disengage] {:08X}: Harbinger refused the ch.7 deny {} at the call (its log says why): he "
                             "runs to the point but may still swing or cast on the way", id, what);
            } else {
                said = fmt::format("{} h={}", what, d.deny);
            }
        }
        d.point  = a_point;
        d.radius = a_radius;
        d.at     = std::chrono::steady_clock::now();
        {
            std::scoped_lock lk(g_disengageMx);
            g_drives[id] = d;
        }
        const RE::NiPoint3 me = a_follower->GetPosition();
        spdlog::info("[disengage] {:08X} '{}': running to {:.0f},{:.0f},{:.0f} ({:.0f}u away, R {:.0f}; ch.24 point+hold h={}, "
                     "ch.7 deny: {})",
                     id, a_follower->GetName() ? a_follower->GetName() : "?", a_point.x, a_point.y, a_point.z,
                     me.GetDistance(a_point), a_radius, d.approach, said);
        return nullptr;
    }

    DisengageState ServiceDisengage(RE::FormID a_follower, std::string& a_why) {
        Drive d;
        {
            std::scoped_lock lk(g_disengageMx);
            const auto it = g_drives.find(a_follower);
            if (it == g_drives.end()) return DisengageState::None;
            d = it->second;
        }
        const auto* api = DisengageApi();
        if (!api || !api->GetCombatApproachState) {
            a_why = "Harbinger is gone";
            return DisengageState::Failed;
        }
        APMF_API::APMF_CombatApproachInfo info{};
        info.size = sizeof(info);
        api->GetCombatApproachState(a_follower, &info);
        const auto age = std::chrono::steady_clock::now() - d.at;
        const bool ours = info.ownerHandle == d.approach;
        if (ours && info.state >= APMF_API::kApproachState_Arrived && info.state <= APMF_API::kApproachState_Refused) {
            a_why = EndName(info.state);
            return DisengageState::Failed;
        }
        if (!ours && age > std::chrono::milliseconds(700)) {
            a_why = "another ch.24 claim owns him (ours never applied)";
            return DisengageState::Failed;
        }
        if (ours && info.state == APMF_API::kApproachState_Holding && info.applied != 0) {
            bool first = false;
            {
                std::scoped_lock lk(g_disengageMx);
                const auto it = g_drives.find(a_follower);
                if (it != g_drives.end() && it->second.approach == d.approach && !it->second.arrived) {
                    it->second.arrived = true;
                    first = true;
                }
            }
            if (first)
                spdlog::info("[disengage] {:08X}: ARRIVED at the point after {:.1f} s (the engine's own inside-the-area answer; "
                             "distance {:.0f}, the area carried the bound {} time(s)). The hold keeps him inside R {:.0f}.",
                             a_follower, std::chrono::duration<float>(age).count(), info.distance, info.applied, d.radius);
            return DisengageState::Arrived;
        }
        if (ours && (info.state == APMF_API::kApproachState_Yielded || info.state == APMF_API::kApproachState_Leashed ||
                     info.state == APMF_API::kApproachState_Disabled) && age > std::chrono::milliseconds(1500)) {
            a_why = info.state == APMF_API::kApproachState_Leashed ? "the point lies outside his ch.23 leash"
                    : info.state == APMF_API::kApproachState_Yielded ? "a package's hold position outranks the bound"
                                                                     : "he has no standard combat area (fleeing, or not high process)";
            return DisengageState::Failed;
        }
        return DisengageState::Running;
    }

    void ReleaseDisengage(RE::FormID a_follower, const char* a_why) {
        Drive d;
        {
            std::scoped_lock lk(g_disengageMx);
            const auto it = g_drives.find(a_follower);
            if (it == g_drives.end()) return;
            d = it->second;
            g_drives.erase(it);
        }
        ReleaseBoth(d);
        spdlog::info("[disengage] {:08X}: drive released (ch.24 h={}, ch.7 h={}; {}) after {:.1f} s{}", a_follower, d.approach,
                     d.deny, a_why ? a_why : "", std::chrono::duration<float>(std::chrono::steady_clock::now() - d.at).count(),
                     d.arrived ? ", he had arrived" : ", he had NOT arrived");
    }

    void ClearDisengage() {
        std::scoped_lock lk(g_disengageMx);
        g_drives.clear();   // revert / load: Harbinger drops every claim of the outgoing world itself
    }

    bool FindStandPoint(RE::FormID a_origin, float a_heading, float a_distance, float a_clearance, RE::NiPoint3& a_out,
                        std::uint32_t& a_status) {
        a_status = APMF_API::kQuery_Unsupported;
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kSpaceMinAbi) return false;
        const auto* v11 = reinterpret_cast<const APMF_API::APMF_API_v11*>(api);
        if (!v11->FindEmptySpace) return false;
        APMF_API::APMF_SpaceQuery q{};
        q.size      = sizeof(q);
        q.origin    = a_origin;
        q.heading   = a_heading;
        q.distance  = a_distance;
        q.clearance = a_clearance;
        q.maxDrop   = 0.0f;   // Harbinger's default (128)
        q.flags     = APMF_API::kSpaceFlag_UseHeading | APMF_API::kSpaceFlag_ClampToWall;
        APMF_API::APMF_SpaceResult r{};
        r.size   = sizeof(r);
        a_status = v11->FindEmptySpace(&q, &r);
        if (a_status != APMF_API::kQuery_Ok) return false;
        a_out = RE::NiPoint3{ r.x, r.y, r.z };
        return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.z);
    }
}
