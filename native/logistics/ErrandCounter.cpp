// logistics/ErrandCounter.cpp -- TOWN PHASE 2 R4 "found at the counter": the learned vendor map, the browsing-hold marker and
// the arrival log lines. See ErrandCounter.h.
//
// PLACEMENT (R4b, Harbinger ABI v28 kIntent_Place): when an errand follower reaches the shop OFF SCREEN and the learned counter is
// CONFIRMED and in an INTERIOR cell, MFO DECLARES a spot beside the vendor and Harbinger moves him (apmf/Place.cpp); the outcome is
// polled on later sim ticks and logged once. MFO adds no raw MoveTo and never falls back to moving him itself: a Refused / Failed
// placement leaves him where the engine has him, logged and unmasked. Exterior counters are NOT placed (APMF-B152).
#include "PCH.h"
#include "ErrandCounter.h"
#include "apmf/APMFBridge.h"
#include "APMF_API.h"

#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Errand::Counter {

    namespace {
        constexpr float kStillRadius = 32.0f;                      // two samples this close = standing at his post
        constexpr auto  kSettleSec   = std::chrono::seconds(2);    // ... and this far apart (the catalog scan runs every plan lap)

        struct Entry {
            Spot  spot;                    // confirmed
            bool  confirmed = false;
            RE::NiPoint3 pending{};        // the last sample, awaiting its confirming twin
            RE::FormID   pendingCell = 0, pendingVendor = 0;
            Clock::time_point pendingAt{};
            bool  pendingSet = false;
        };

        // Placement stands past the offset: 70 u in FRONT of the vendor's learned heading. A shop counter is ~50-100 u deep, so the
        // customer's side of it is ~70 u out; two actor hulls (~22 u radius each) need >44 u, so he is never inside the vendor, and
        // it is short enough to stay on the vendor's side of any wall behind a counter-front. Z 0: the vendor's own floor height.
        constexpr float kFrontOffset = 70.0f;
        constexpr auto  kPlaceQueuedGiveUp = std::chrono::seconds(60);   // a placement still Queued this long: say so, stop polling

        struct PendingPlace { std::uint32_t handle = 0; Clock::time_point at{}; std::string vendorName; };

        std::mutex                                  g_mx;
        std::unordered_map<RE::FormID, PendingPlace> g_placing;   // follower -> the placement being polled (g_mx)
        std::unordered_map<RE::FormID, Entry>       g_map;      // vendor FACTION -> counter
    }

    void Learn(const RE::Actor* a_vendor, RE::FormID a_faction, Clock::time_point a_now) {
        if (!a_vendor || !a_faction) return;
        const auto* cell = a_vendor->GetParentCell();
        if (!cell) return;
        const auto pos = a_vendor->GetPosition();
        const auto cid = cell->GetFormID();
        const auto vid = a_vendor->GetFormID();
        bool logNew = false, logLost = false;
        {
            std::scoped_lock lk(g_mx);
            auto& e = g_map[a_faction];
            // Does this sample agree with the CONFIRMED spot? A different vendor actor, another cell or a move disagrees: the
            // spot is dropped until a new pair confirms (a stale counter is worse than none).
            if (e.confirmed && (e.spot.vendor != vid || e.spot.cell != cid || e.spot.pos.GetDistance(pos) > kStillRadius)) {
                e.confirmed = false;
                logLost = true;
            }
            const bool twin = e.pendingSet && e.pendingVendor == vid && e.pendingCell == cid &&
                              e.pending.GetDistance(pos) <= kStillRadius && a_now - e.pendingAt >= kSettleSec;
            if (twin) {
                logNew = !e.confirmed;
                e.spot.vendor = vid;
                e.spot.cell = cid;
                e.spot.pos = pos;
                e.spot.yaw = a_vendor->GetAngleZ();
                e.confirmed = true;
                e.pendingSet = false;
            } else if (!e.pendingSet || e.pendingVendor != vid || e.pendingCell != cid || e.pending.GetDistance(pos) > kStillRadius) {
                e.pending = pos; e.pendingCell = cid; e.pendingVendor = vid; e.pendingAt = a_now; e.pendingSet = true;   // restart the confirmation
            }
        }
        if (logLost)
            spdlog::info("[errand] counter DROPPED for shop faction {:08X}: vendor {:08X} no longer stands where it was confirmed; unknown until a new pair of samples agrees",
                         a_faction, vid);
        if (logNew)
            spdlog::info("[errand] counter LEARNED for shop faction {:08X}: vendor {:08X} stands at ({:.0f},{:.0f},{:.0f}) yaw {:.2f} in cell {:08X}",
                         a_faction, vid, pos.x, pos.y, pos.z, a_vendor->GetAngleZ(), cid);
    }

    bool Lookup(RE::FormID a_faction, Spot& a_out) {
        std::scoped_lock lk(g_mx);
        const auto it = g_map.find(a_faction);
        if (it == g_map.end() || !it->second.confirmed) return false;
        a_out = it->second.spot;
        return true;
    }

    void NoteResume(RE::FormID a_follower, const std::string& a_name, const RE::Actor* a_follower3d, const RE::Actor* a_vendor,
                    RE::FormID a_faction) {
        Spot s;
        if (!Lookup(a_faction, s)) {
            spdlog::info("[errand] {:08X} '{}': counter unknown for shop faction {:08X} (that vendor was never seen standing still while "
                         "loaded); he is found where the engine left him, no teleport", a_follower, a_name, a_faction);
            return;
        }
        const float dCounter = a_follower3d ? a_follower3d->GetPosition().GetDistance(s.pos) : -1.0f;
        const float dVendor = (a_follower3d && a_vendor) ? a_follower3d->GetPosition().GetDistance(a_vendor->GetPosition()) : -1.0f;
        spdlog::info("[errand] {:08X} '{}': counter known for shop faction {:08X} ({:.0f},{:.0f},{:.0f}); found {:.0f} u from it, {:.0f} u from the "
                     "vendor now (-1 = n/a); browsing hold next", a_follower, a_name, a_faction, s.pos.x, s.pos.y, s.pos.z, dCounter, dVendor);
    }

    void NoteSimArrival(RE::FormID a_follower, const std::string& a_name, const std::string& a_vendorName, RE::FormID a_faction) {
        Spot s;
        const bool known = Lookup(a_faction, s);
        spdlog::info("[errand] {:08X} '{}': OUT_SIM arrival at '{}' -- counter {}", a_follower, a_name, a_vendorName,
                     known ? std::format("known ({:.0f},{:.0f},{:.0f}) cell {:08X}", s.pos.x, s.pos.y, s.pos.z, s.cell)
                           : std::string("unknown; he stays where the engine has him (no placement)"));
        if (!known) return;
        const auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(s.cell);
        if (!cell) {
            spdlog::info("[errand] {:08X} '{}': counter cell {:08X} does not resolve: not placed", a_follower, a_name, s.cell);
            return;
        }
        if (!cell->IsInteriorCell()) {
            spdlog::info("[errand] {:08X} '{}': exterior counter: not placed (APMF-B152)", a_follower, a_name);
            return;
        }
        if (!MFO::APMFBridge::PlaceSupported()) {   // Harbinger absent = errands do not run; ABI < 28 is logged once by the wrapper
            MFO::APMFBridge::RequestPlace(a_follower, s.vendor, {}, 0.0f, 0);
            return;
        }
        // Stand across the counter from the vendor: in front of his heading (Skyrim yaw 0 = north, clockwise: forward = (sin, cos)),
        // facing back at him. The offset is in WORLD axes (Papyrus MoveTo semantics).
        constexpr float kTwoPi = 6.28318530718f;
        const RE::NiPoint3 off(std::sin(s.yaw) * kFrontOffset, std::cos(s.yaw) * kFrontOffset, 0.0f);
        float yaw = std::fmod(s.yaw + 3.14159265359f, kTwoPi);
        if (yaw < 0.0f) yaw += kTwoPi;
        // NEVER request a placement of a LOADED actor (APMF-B149, unobserved): OnlyIfUnloaded is the guard. RefuseIfPlayerPresent
        // keeps it unseen. Both always; SetYaw so he faces the vendor.
        constexpr std::uint32_t flags = APMF_API::kPlace_OnlyIfUnloaded | APMF_API::kPlace_RefuseIfPlayerPresent | APMF_API::kPlace_SetYaw;
        const std::uint32_t h = MFO::APMFBridge::RequestPlace(a_follower, s.vendor, off, yaw, flags);
        if (h == 0) {
            spdlog::error("[errand] {:08X} '{}': placement REQUEST REFUSED at the call (Harbinger names why in APMF.log); he stays where the "
                          "engine has him", a_follower, a_name);
            return;
        }
        spdlog::info("[errand] {:08X} '{}': placement requested beside vendor {:08X} (offset {:.0f},{:.0f} = {:.0f} u in front of his heading, "
                     "yaw {:.2f} facing him), handle {}", a_follower, a_name, s.vendor, off.x, off.y, kFrontOffset, yaw, h);
        std::scoped_lock lk(g_mx);
        g_placing[a_follower] = PendingPlace{h, Clock::now(), a_vendorName};
    }

    void PollPlacement(RE::FormID a_follower, const std::string& a_name) {
        PendingPlace pp;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_placing.find(a_follower);
            if (it == g_placing.end()) return;
            pp = it->second;
        }
        const auto r = MFO::APMFBridge::ReadPlace(pp.handle);
        bool done = true;
        switch (r.state) {
        case APMF_API::kPlaceState_Queued:
            done = Clock::now() - pp.at >= kPlaceQueuedGiveUp;
            if (done)
                spdlog::warn("[errand] {:08X} '{}': placement handle {} still Queued after {} s -- no outcome read", a_follower, a_name,
                             pp.handle, kPlaceQueuedGiveUp.count());
            break;
        case APMF_API::kPlaceState_Placed:
            spdlog::info("[errand] {:08X} '{}': PLACED at '{}' ({:.0f},{:.0f},{:.0f}) cell {:08X}", a_follower, a_name, pp.vendorName,
                         r.pos.x, r.pos.y, r.pos.z, r.cell);
            break;
        case APMF_API::kPlaceState_Refused:
            spdlog::warn("[errand] {:08X} '{}': placement REFUSED (why {}, blocker {}) -- he stays where the engine has him", a_follower,
                         a_name, r.why, r.blocker);
            break;
        case APMF_API::kPlaceState_Failed:
            spdlog::error("[errand] {:08X} '{}': placement FAILED (why {}) cell {:08X} -- he stays where the engine has him", a_follower,
                          a_name, r.why, r.cell);
            break;
        default:   // Unknown: dropped by a load / new game, or the grace ran out
            spdlog::warn("[errand] {:08X} '{}': placement handle {} reads Unknown (dropped by a load, or the record aged out)", a_follower,
                         a_name, pp.handle);
            break;
        }
        if (!done) return;
        std::scoped_lock lk(g_mx);
        const auto it = g_placing.find(a_follower);
        if (it != g_placing.end() && it->second.handle == pp.handle) g_placing.erase(it);
    }

    void Clear() {
        std::scoped_lock lk(g_mx);
        g_map.clear();
        g_placing.clear();
    }
}
