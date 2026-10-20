// logistics/ErrandCounter.cpp -- TOWN PHASE 2 R4 "found at the counter": the learned vendor map, the browsing-hold marker and
// the arrival log lines. See ErrandCounter.h.
//
// PLACEMENT IS NOT BUILT (marth: "we are porting everything to harbinger, so having MFO do something will just need redoing
// later"). The design's OUT_SIM `tArrive` MoveTo would put an unloaded follower beside the vendor, but Harbinger (ABI v27,
// kIntent_* 1..27, Docs/FACETS.md) has NO placement / teleport facet: ch.19 WALKS a loaded actor, nothing moves an unloaded
// one. MFO adds no raw MoveTo. NoteSimArrival says so, once per errand, so the field log shows which counter the follower
// WOULD have been placed at once the facet exists (the Lookup below is the input it needs).
#include "PCH.h"
#include "ErrandCounter.h"

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Errand::Counter {

    namespace {
        constexpr float kStillRadius = 32.0f;                      // two samples this close = standing at his post
        constexpr auto  kSettleSec   = std::chrono::seconds(2);    // ... and this far apart (the catalog scan runs every plan lap)

        struct Entry {
            Spot  spot;                    // confirmed
            bool  confirmed = false;
            RE::NiPoint3 pending{};        // the last sample, awaiting its confirming twin
            RE::FormID   pendingCell = 0;
            Clock::time_point pendingAt{};
            bool  pendingSet = false;
        };

        std::mutex                                  g_mx;
        std::unordered_map<RE::FormID, Entry>       g_map;      // vendor FACTION -> counter
        std::unordered_set<RE::FormID>              g_browse;   // followers holding the browsing beat
    }

    void Learn(const RE::Actor* a_vendor, RE::FormID a_faction, Clock::time_point a_now) {
        if (!a_vendor || !a_faction) return;
        const auto* cell = a_vendor->GetParentCell();
        if (!cell) return;
        const auto pos = a_vendor->GetPosition();
        const auto cid = cell->GetFormID();
        bool logNew = false, logMoved = false;
        {
            std::scoped_lock lk(g_mx);
            auto& e = g_map[a_faction];
            const bool twin = e.pendingSet && e.pendingCell == cid && e.pending.GetDistance(pos) <= kStillRadius &&
                              a_now - e.pendingAt >= kSettleSec;
            if (twin) {
                const bool changed = e.confirmed && (e.spot.cell != cid || e.spot.pos.GetDistance(pos) > kStillRadius);
                logNew = !e.confirmed;
                logMoved = changed;
                e.spot.vendor = a_vendor->GetFormID();
                e.spot.cell = cid;
                e.spot.pos = pos;
                e.spot.yaw = a_vendor->GetAngleZ();
                e.confirmed = true;
                e.pendingSet = false;
            } else if (!e.pendingSet || e.pendingCell != cid || e.pending.GetDistance(pos) > kStillRadius) {
                e.pending = pos; e.pendingCell = cid; e.pendingAt = a_now; e.pendingSet = true;   // restart the confirmation
            }
        }
        if (logNew || logMoved)
            spdlog::info("[errand] counter {} for shop faction {:08X}: vendor {:08X} stands at ({:.0f},{:.0f},{:.0f}) yaw {:.2f} in cell {:08X}",
                         logMoved ? "MOVED" : "LEARNED", a_faction, a_vendor->GetFormID(), pos.x, pos.y, pos.z, a_vendor->GetAngleZ(), cid);
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
        spdlog::info("[errand] {:08X} '{}': OUT_SIM arrival at '{}' -- counter {}; NOT placed: Harbinger has no placement facet "
                     "(ch.19 walks a loaded actor only) and MFO adds no raw MoveTo", a_follower, a_name, a_vendorName,
                     known ? std::format("known ({:.0f},{:.0f},{:.0f}) cell {:08X}", s.pos.x, s.pos.y, s.pos.z, s.cell)
                           : std::string("unknown"));
    }

    void MarkBrowse(RE::FormID a_follower) { std::scoped_lock lk(g_mx); g_browse.insert(a_follower); }
    bool Browsing(RE::FormID a_follower)   { std::scoped_lock lk(g_mx); return g_browse.contains(a_follower); }
    void EndBrowse(RE::FormID a_follower)  { std::scoped_lock lk(g_mx); g_browse.erase(a_follower); }

    void Clear() {
        std::scoped_lock lk(g_mx);
        g_map.clear();
        g_browse.clear();
    }
}
