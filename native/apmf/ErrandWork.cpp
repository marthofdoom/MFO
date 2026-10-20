// apmf/ErrandWork.cpp -- the Harbinger claim of a TOWN ERRAND's WORK (TOWN PHASE 2 R7: inn + temple; ClickUp 86e3t1mx7).
// The decisions (which stop, when to sit, when to eat, when to pray) are logistics/ErrandRest.cpp's; this file owns only the
// ch.9 package-offer claim an errand's work rides and its run-time input write:
//
//   * ch.9 kIntent_OfferPackage (T3 0x49) -- the MFO WORK package (Forms.h MFO_ErrandActivate/Sandbox/Eat/Sleep, riding the
//     vanilla templates 00019B2D / 0001C254 / 00019714 / 00019717) is handed to the follower DIRECTLY by Harbinger's 0x49 hook,
//     composed with his own AI (principle 3: moderate per facet, never substitute). One claim per follower; a CHANGE of work
//     (Eat -> Sandbox -> Sleep) is a Repoint of the same handle.
//   * THE RUN-TIME WRITE. The shrine / the inn's ref cannot be authored (the refs are not persistent), so the target / location
//     input of the follower's SLOT record is written here, BEFORE the claim is filed (Packages::SetPackageTargetRef /
//     SetPackageLocation, the same guarded writer the loot-travel Location uses). A write that fails its layout guard files
//     NOTHING and the call returns false: the errand skips the stop, loudly (principle 7). One slot per concurrent follower
//     (kErrandWorkSlots = 4): two followers never share a record; the slot is freed only AFTER the claim is released.
//
// ABI. Needs ABI >= 24 like every town errand (APMFBridge::ErrandSupported). The ch.19 walk claim of the same errand stays filed
// (a leg that ends drops only its own internal offer, APMF_API.h "A LEG ENDS BUT THE CLAIM STANDS"); the work package is offered
// after the leg Arrived, so the two never compete for the offer at one moment.
//
// THREADING. Worker (the service tick / the errand sweep) and the revert with the pump drained. Own mutex, never the bridge's
// g_mx (this file never takes g_mx). APMF's RequestEx / Repoint / Release / IsClaimLive are safe from any thread (APMF_API.h).
// Packages::SetPackage* write package memory on the worker before the claim exists (the same road SetAPMFLootTravelTarget takes
// from LootTravelFill, a worker too). NOTHING IS SAVED: APMF drops every claim on a load; ClearErrandWork drops the table.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"   // g_apmf, kOwnBasis
#include "APMF_API.h"
#include "Forms.h"
#include "Packages.h"

#include <array>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kWorkMinAbi = 24;   // the town-errand floor
        constexpr auto kPublishGrace = std::chrono::seconds(3);

        struct Work {
            APMF_API::Handle handle = APMF_API::kInvalidHandle;
            int              slot = -1;
            ErrandWorkKind   kind = ErrandWorkKind::Sandbox;
            RE::FormID       ref = 0;
            RE::FormID       package = 0;
            bool             everLive = false;
            std::chrono::steady_clock::time_point filed{};
        };
        std::mutex                               s_mx;
        std::unordered_map<RE::FormID, Work>     s_work;               // keyed by follower; guarded by s_mx
        std::array<RE::FormID, Forms::kErrandWorkSlots> s_slotOwner{};  // slot -> follower (0 = free); guarded by s_mx

        const APMF_API::APMF_API_v24* WorkApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kWorkMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v24*>(api);
        }

        Forms::ErrandWorkKind ToForms(ErrandWorkKind a_k) {
            switch (a_k) {
                case ErrandWorkKind::Activate: return Forms::ErrandWorkKind::Activate;
                case ErrandWorkKind::Eat:      return Forms::ErrandWorkKind::Eat;
                case ErrandWorkKind::Sleep:    return Forms::ErrandWorkKind::Sleep;
                default:                       return Forms::ErrandWorkKind::Sandbox;
            }
        }
        // The TEMPLATE's own input name for each kind (FindInput is by NAME, never by index): Activate's runtime input is its
        // Target; the other three are Locations (Sandbox declares three "Location" names, Packages resolves the uid with a slot).
        const char* InputName(ErrandWorkKind a_k) {
            switch (a_k) {
                case ErrandWorkKind::Activate: return "Target";
                case ErrandWorkKind::Eat:      return "Eat Location";
                case ErrandWorkKind::Sleep:    return "Sleep Location";
                default:                       return "Location";
            }
        }
        const char* KindName(ErrandWorkKind a_k) {
            switch (a_k) {
                case ErrandWorkKind::Activate: return "Activate";
                case ErrandWorkKind::Eat:      return "Eat";
                case ErrandWorkKind::Sleep:    return "Sleep";
                default:                       return "Sandbox";
            }
        }

        // s_mx HELD.
        void FreeLocked(RE::FormID a_follower) {
            const auto it = s_work.find(a_follower);
            if (it == s_work.end()) return;
            if (it->second.slot >= 0 && it->second.slot < static_cast<int>(s_slotOwner.size()) &&
                s_slotOwner[it->second.slot] == a_follower)
                s_slotOwner[it->second.slot] = 0;
            s_work.erase(it);
        }
    }

    const char* ErrandWorkName(ErrandWorkKind a_kind) { return KindName(a_kind); }

    bool OfferErrandWork(RE::FormID a_follower, ErrandWorkKind a_kind, RE::TESObjectREFR* a_ref, float a_radius, std::string& a_why) {
        a_why.clear();
        auto* api = WorkApi();
        if (!api) { a_why = "Harbinger absent or older than ABI 24"; return false; }
        if (a_follower == 0 || !a_ref) { a_why = "no follower / no target reference"; return false; }
        std::scoped_lock lock(s_mx);
        auto it = s_work.find(a_follower);
        // A claim Harbinger no longer holds (a refused / superseded offer) is forgotten; the slot is kept for the follower.
        if (it != s_work.end() && it->second.handle != APMF_API::kInvalidHandle) {
            if (api->IsClaimLive(it->second.handle)) it->second.everLive = true;
            else if (it->second.everLive || std::chrono::steady_clock::now() - it->second.filed > kPublishGrace) {
                spdlog::info("[errand-work] {:08X}: the ch.9 offer claim (handle {}) is no longer live -- filing a fresh one", a_follower,
                             it->second.handle);
                it->second.handle = APMF_API::kInvalidHandle;
                it->second.everLive = false;
            }
        }
        int slot = it != s_work.end() ? it->second.slot : -1;
        if (slot < 0) {
            for (int i = 0; i < static_cast<int>(s_slotOwner.size()); ++i)
                if (s_slotOwner[i] == 0) { slot = i; break; }
            if (slot < 0) { a_why = "all four errand work slots are held by other followers"; return false; }
        }
        auto* pkg = Forms::ErrandWorkPackage(ToForms(a_kind), slot);
        if (!pkg) { a_why = std::string("the ") + KindName(a_kind) + " work package did not resolve (old MFO.esp?)"; return false; }

        // THE RUN-TIME WRITE, before the claim (the claim names a record whose input is already right).
        int uid = -1;
        const bool wrote = a_kind == ErrandWorkKind::Activate ? Packages::SetPackageTargetRef(pkg, InputName(a_kind), a_ref)
                                                              : Packages::SetPackageLocation(pkg, InputName(a_kind), a_ref, a_radius, uid);
        if (!wrote) { a_why = std::string("the ") + KindName(a_kind) + " package input write declined (layout guard; see [pkg])"; return false; }

        APMF_API::APMF_Param p{};
        p.form = pkg->GetFormID();
        auto& w = s_work[a_follower];
        const bool fresh = w.handle == APMF_API::kInvalidHandle;
        if (fresh) {
            w.handle = api->RequestEx(a_follower, APMF_API::kIntent_OfferPackage, kOwnBasis, &p);
            if (w.handle == APMF_API::kInvalidHandle) {
                // Refused at the call. The slot is released with the (empty) record, nothing is left half-filed.
                FreeLocked(a_follower);
                a_why = "Harbinger REFUSED the ch.9 package-offer claim (APMF.log names why)";
                return false;
            }
            w.everLive = false;
            w.filed = std::chrono::steady_clock::now();
        } else if (w.package != p.form) {
            api->Repoint(w.handle, &p);   // a change of work: the same claim moves to the next record
        }
        const bool changed = fresh || w.package != p.form || w.ref != a_ref->GetFormID();
        w.slot = slot; w.kind = a_kind; w.ref = a_ref->GetFormID(); w.package = p.form;
        s_slotOwner[slot] = a_follower;
        if (changed)
            spdlog::info("[errand-work] {:08X}: ch.9 OFFER {} -> package {:08X} (slot {}) at ref {:08X} radius {:.0f}{} handle {}", a_follower,
                         KindName(a_kind), p.form, slot, w.ref, a_radius, uid >= 0 ? std::format(", input uid {}", uid) : std::string{},
                         w.handle);
        return true;
    }

    bool ErrandWorkHeld(RE::FormID a_follower) {
        auto* api = WorkApi();
        std::scoped_lock lock(s_mx);
        const auto it = s_work.find(a_follower);
        return api && it != s_work.end() && it->second.handle != APMF_API::kInvalidHandle &&
               (api->IsClaimLive(it->second.handle) || std::chrono::steady_clock::now() - it->second.filed <= kPublishGrace);
    }

    void ReleaseErrandWork(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_mx);
        const auto it = s_work.find(a_follower);
        if (it == s_work.end()) return;
        if (api && it->second.handle != APMF_API::kInvalidHandle) api->Release(it->second.handle);   // the claim first ...
        spdlog::info("[errand-work] {:08X}: ch.9 offer RELEASED ({} slot {})", a_follower, KindName(it->second.kind), it->second.slot);
        FreeLocked(a_follower);                                                                       // ... the slot after
    }

    void ClearErrandWork() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_mx);
        for (auto& [id, w] : s_work)
            if (api && w.handle != APMF_API::kInvalidHandle) api->Release(w.handle);
        s_work.clear();
        s_slotOwner.fill(0);
    }
}
