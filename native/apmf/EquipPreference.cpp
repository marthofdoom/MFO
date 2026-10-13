// apmf/EquipPreference.cpp -- the ch.26 EQUIP PREFERENCE claim (ABI v22, kIntent_EquipPreference):
// MFO's in-combat weapon / shield choice per hand, declared to Harbinger as a ranked preference that
// Harbinger lifts inside the engine's own CalculateScore (weighting phase 1, W1; marth 2026-10-12:
// "Direct equip is going. Harbinger has all the responsibility now."). The claim lifecycle and the
// send-on-change detector only: WHAT is declared is decided by cast/EquipWeigh.cpp from the force-hold
// ledger. Contract: APMFBridge.h; Harbinger's APMF_API.h "ABI v22: EQUIP PREFERENCE" and its FACETS.md
// ch.26.
//
// THREADING. Every caller is the AddTask job worker (the Scheduler's reconcile lap, the equip gambits,
// ReleaseForcedWeapon) except ClearEquipPreferences (kPreLoadGame / revert, a drained pump) and the
// read-only state query (any thread). The table has its own mutex; ClearEquipPreferences runs inside
// ClearTransientState under g_mx (order g_mx -> g_prefMx), and nothing here ever takes g_mx.
// Harbinger's SetEquipPreference / Repoint / Release / RequestEx are thread-safe enqueues that never
// call back into MFO, so they are made under g_prefMx (the ch.17 DeclareEquipSet shape).
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kPrefMinAbi = 22;   // the ABI that first serves intent 26
        // A refused claim is asked again no sooner than this (Harbinger refuses on configuration:
        // EnableScoreSteer=0, no score seat; a load does not change it, a re-ask every lap would be
        // log noise and an enqueue per lap).
        constexpr auto kRefusedRetry = std::chrono::seconds(30);

        struct Pref {
            APMF_API::Handle                         handle = APMF_API::kInvalidHandle;
            std::vector<APMF_API::APMF_EquipPrefEntry> sent;     // the declaration the handle carries
            bool                                     force  = false;
            std::chrono::steady_clock::time_point    mintedAt{};
            std::chrono::steady_clock::time_point    refusedAt{};   // last refusal (0 = none this streak)
        };
        std::mutex                           g_prefMx;
        std::unordered_map<RE::FormID, Pref> g_prefs;   // guarded by g_prefMx

        const APMF_API::APMF_API_v22* PrefApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kPrefMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v22*>(api);
        }

        bool SameEntries(const std::vector<APMF_API::APMF_EquipPrefEntry>& a,
                         const std::vector<APMF_API::APMF_EquipPrefEntry>& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (a[i].form != b[i].form || a[i].slot != b[i].slot || a[i].rank != b[i].rank) return false;
            return true;
        }

        const char* SlotName(std::uint8_t a_slot) {
            return a_slot == APMF_API::kEquipSlot_Right ? "R" : a_slot == APMF_API::kEquipSlot_Left ? "L" : "any";
        }

        std::string Describe(const std::vector<APMF_API::APMF_EquipPrefEntry>& a_entries) {
            std::string s;
            for (const auto& e : a_entries) {
                const auto* f = RE::TESForm::LookupByID(e.form);
                const char* n = f ? f->GetName() : nullptr;
                if (!s.empty()) s += ", ";
                s += std::format("{}:'{}' {:08X} rank {}", SlotName(e.slot), (n && *n) ? n : "?", e.form, e.rank);
            }
            return s.empty() ? std::string("none") : s;
        }

        // Release one entry's handle. Caller holds g_prefMx.
        void ReleaseLocked(const APMF_API::APMF_API_v22* a_api, Pref& a_p) {
            if (a_api && a_p.handle != APMF_API::kInvalidHandle) a_api->Release(a_p.handle);
            a_p.handle = APMF_API::kInvalidHandle;
            a_p.sent.clear();
            a_p.force = false;
        }
    }

    bool EquipPreferenceSupported() { return PrefApi() != nullptr && EquipAuthoritySupported(); }

    bool DeclareEquipPreference(RE::FormID a_follower, const std::vector<APMF_API::APMF_EquipPrefEntry>& a_entries,
                                const char* a_why) {
        const auto* api = PrefApi();
        if (!api || a_follower == 0 || a_follower == 0x14) return false;   // Harbinger refuses the player
        const char* why = a_why ? a_why : "?";
        std::scoped_lock lock(g_prefMx);
        if (a_entries.empty()) {
            const auto it = g_prefs.find(a_follower);
            if (it == g_prefs.end()) return false;
            const bool had = it->second.handle != APMF_API::kInvalidHandle;
            ReleaseLocked(api, it->second);
            g_prefs.erase(it);
            if (had) spdlog::info("[equip-pref] {:08X}: preference cleared, ch.26 released ({})", a_follower, why);
            return false;
        }
        auto& p = g_prefs[a_follower];
        const auto now = std::chrono::steady_clock::now();
        // A handle old enough to be in Harbinger's published snapshot is re-validated: a claim Harbinger
        // ended (its sweep, a release-all hotkey, an unload) took its declaration with it, and
        // SetEquipPreference on it is a silent no-op (the ch.17 F2 rule, kEquipAuthValidateAfter).
        if (p.handle != APMF_API::kInvalidHandle && now - p.mintedAt >= kEquipAuthValidateAfter &&
            !api->IsClaimLive(p.handle)) {
            spdlog::warn("[equip-pref] {:08X}: ch.26 handle {} is no longer live on Harbinger's side (its "
                         "declaration went with it) -- re-claiming", a_follower, p.handle);
            p.handle = APMF_API::kInvalidHandle;   // no Release: Harbinger already forgot it
            p.sent.clear();
            p.force = false;
        }
        if (p.handle == APMF_API::kInvalidHandle) {
            if (p.refusedAt.time_since_epoch().count() != 0 && now - p.refusedAt < kRefusedRetry) return false;
            APMF_API::APMF_Param prm{};
            prm.ival = static_cast<std::int32_t>(APMF_API::kEquipPref_None);   // score lift only (D4: force on a miss)
            p.handle = api->RequestEx(a_follower, APMF_API::kIntent_EquipPreference, kOwnBasis, &prm);
            if (p.handle == APMF_API::kInvalidHandle) {
                // NO DECLINE-FALLBACK (marth 2026-09-28; the standing "legacy = APMF-absent only" rule): the
                // placement still rides ch.17, there is no score lift, nothing MFO equips directly.
                if (p.refusedAt.time_since_epoch().count() == 0)
                    spdlog::error("[equip-pref] {:08X}: Harbinger REFUSED the ch.26 equip-preference claim "
                                  "(EnableScoreSteer=0, no score seat on this runtime, or the actor; APMF.log says "
                                  "which) -- NO score lift for him; placement still rides the ch.17 declaration and "
                                  "MFO makes no direct equip. Asked again in {} s.", a_follower,
                                  std::chrono::duration_cast<std::chrono::seconds>(kRefusedRetry).count());
                p.refusedAt = now;
                return false;
            }
            if (p.refusedAt.time_since_epoch().count() != 0)
                spdlog::info("[equip-pref] {:08X}: ch.26 claim taken after an earlier refusal", a_follower);
            p.refusedAt = {};
            p.mintedAt  = now;
            p.sent.clear();
            p.force = false;
            spdlog::info("[equip-pref] {:08X}: ch.26 claim (kIntent_EquipPreference, basis {}, handle {})",
                         a_follower, kOwnBasis, p.handle);
        }
        if (!p.sent.empty() && SameEntries(p.sent, a_entries)) return true;   // unchanged: nothing sent
        const std::uint32_t count = static_cast<std::uint32_t>(
            std::min<std::size_t>(a_entries.size(), APMF_API::kMaxEquipPreference));
        if (a_entries.size() > APMF_API::kMaxEquipPreference)
            spdlog::error("[equip-pref] {:08X}: {} preferred items, over kMaxEquipPreference {} -- truncated "
                          "(MFO's composition bug)", a_follower, a_entries.size(), APMF_API::kMaxEquipPreference);
        // COPIED inside the call (APMF_API.h: entries read synchronously, never retained).
        api->SetEquipPreference(p.handle, a_entries.data(), count);
        p.sent.assign(a_entries.begin(), a_entries.begin() + count);
        spdlog::info("[equip-pref] {:08X}: DECLARE {} ({})", a_follower, Describe(p.sent), why);
        return true;
    }

    void ReleaseEquipPreference(RE::FormID a_follower, const char* a_why) {
        const auto* api = PrefApi();
        std::scoped_lock lock(g_prefMx);
        const auto it = g_prefs.find(a_follower);
        if (it == g_prefs.end()) return;
        const bool had = it->second.handle != APMF_API::kInvalidHandle;
        ReleaseLocked(api, it->second);
        g_prefs.erase(it);
        if (had) spdlog::info("[equip-pref] {:08X}: ch.26 released ({})", a_follower, a_why ? a_why : "?");
    }

    std::uint32_t EquipPreferenceState(RE::FormID a_follower, APMF_API::APMF_EquipPrefInfo* a_out) {
        const auto* api = PrefApi();
        if (!api || a_follower == 0) return APMF_API::kEquipPrefState_None;
        if (a_out) {
            std::memset(a_out, 0, sizeof(*a_out));
            a_out->size = sizeof(APMF_API::APMF_EquipPrefInfo);
        }
        return api->GetEquipPreferenceState(a_follower, a_out);
    }

    bool SetEquipPreferenceForce(RE::FormID a_follower, bool a_force) {
        const auto* api = PrefApi();
        if (!api) return false;
        std::scoped_lock lock(g_prefMx);
        const auto it = g_prefs.find(a_follower);
        if (it == g_prefs.end() || it->second.handle == APMF_API::kInvalidHandle || it->second.force == a_force)
            return false;
        APMF_API::APMF_Param prm{};
        prm.ival = static_cast<std::int32_t>(a_force ? APMF_API::kEquipPref_Force : APMF_API::kEquipPref_None);
        api->Repoint(it->second.handle, &prm);   // Harbinger's OnOwnerChanged re-reads ival (and logs it loudly)
        it->second.force = a_force;
        return true;
    }

    bool EquipPreferenceForced(RE::FormID a_follower) {
        std::scoped_lock lock(g_prefMx);
        const auto it = g_prefs.find(a_follower);
        return it != g_prefs.end() && it->second.handle != APMF_API::kInvalidHandle && it->second.force;
    }

    void ClearEquipPreferences() {
        const auto* api = PrefApi();
        std::scoped_lock lock(g_prefMx);
        for (auto& [fid, p] : g_prefs) ReleaseLocked(api, p);   // Harbinger drops them at its own kPreLoadGame: harmless
        g_prefs.clear();
    }

}
