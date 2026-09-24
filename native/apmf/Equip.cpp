// apmf/Equip.cpp -- the EQUIPMENT claims: the ch.15 weapon-order equipment
// claim (ClaimEquipment / WeaponHandActive) and the ch.17 EQUIP AUTHORITY
// (ClaimEquipAuthority, DeclareEquipScope / DeclareEquipSet).
// Split out of the old native/APMFBridge.cpp by the wave-1 subsystem-folder split
// (2026-09-24): a pure move, proven function by function with tools/splitcheck.
// The bridge's shared claim state lives in apmf/APMFBridge_internal.h.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"   // wave-1 split: the bridge's shared claim state
#include "APMF_API.h"
#include "ComposedCast.h"   // ObservedFiring (the idle-hand floor's unobserved gate) + ClearWatchHand
#include "cast/Actuation.h"     // CastInFlightOnHand -- THE one in-flight definition; the idle-hand
                           // floor's gate is armed on CHARGE, not on claim age alone (2026-09-22)
                            // (the expiry sweep drops the swept claim's [cfc] watch) -- both called
                            // from Tick() ONLY, which runs inside the AddTask job-worker body
                            // (Diagnostics.cpp) = ComposedCast's own serialized context; ComposedCast
                            // never takes g_mx, so calling it under g_mx cannot deadlock.
#include "Config.h"
#include "Followers.h"   // g_active.size() -- round-robin-aware expiry sizing (FacetExpiry)
#include "Loadout.h"     // WeaponHandActive's live-grip read
#include "MainThread.h"
#include "Packages.h"   // kMaxLootSlots -- the ch.19 loot-travel leg table is keyed by loot SLOT
#include "Rapport.h"

#include <array>   // F10: the two-slot (driving / idle-hand floor) log throttles
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>   // ch.17 claim-refusal throttle
#include <algorithm>       // ch.8 allow-list: sort + unique over the published form set

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    // ── weapon-order equipment (per-order, ch.15) ───────────────────────────────
    void ClaimEquipment(RE::FormID a_follower, RE::FormID a_weaponForm) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_weaponForm == 0 || !Config::g_weaponStyleControl.load()) return;
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureClaimLocked(api, a_follower, APMF_API::kIntent_Equipment, o.equipHandle, o.equip, a_weaponForm);
        o.equipRefreshed = std::chrono::steady_clock::now();
        EraseIfEmpty(g_owned.find(a_follower));
    }

    void ReleaseEquipment(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseHandleLocked(it->second.equipHandle, it->second.equip);
        EraseIfEmpty(it);
    }

    bool IsEquipmentClaimActive(RE::FormID a_follower) {
        // Fast-out before the lock: APMF absent -> never active (mirrors
        // IsOwnedCastActive exactly, so the EquipGateThunk caller's native
        // enforcement path is byte-identical when APMF is not present).
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() && it->second.equipHandle != APMF_API::kInvalidHandle;
    }

    bool WeaponHandActive(RE::Actor* a_follower) {
        if (!a_follower) return false;
        // Live read FIRST -- cheap, and correct even when APMF is absent (a
        // legacy-hybrid follower with a sword out is still weapon-active with
        // no equip-gambit claim in the picture at all).
        const auto grip = Loadout::Read(a_follower, nullptr).grip;
        if (grip == Loadout::Grip::OneHanded || grip == Loadout::Grip::TwoHanded) return true;
        // Momentarily-empty-handed race guard: an equip gambit holding a live
        // force-equip claim will reassert the weapon shortly even though the
        // hand reads free THIS tick (the deck-proven failure this exists to
        // close -- see this function's header doc).
        return IsEquipmentClaimActive(a_follower->GetFormID());
    }

    // ── EQUIP AUTHORITY (ch.17, APMF v7; feat/mfo-equip-authority) ────────────
    // See APMFBridge.h's block doc. Worker road, g_mx for the map, APMF calls are
    // thread-safe enqueues. The three non-arbitration early returns here and in
    // EquipAuthoritySupported() read the SAME g_apmf / abiVersion / toggle, so a
    // `false` from Claim/Declare with Supported() true can only mean APMF refused.
    // ABI FLOOR = 9 (SetEquipScope). Under v8 MFO's declaration OMITS the hands
    // it does not hold, and a v8 APMF owns every category by default -- so a v9
    // declaration on a v8 APMF would refuse every engine weapon equip on the
    // actor (the F6 freeze, worse than before). A too-old APMF therefore gets NO
    // authority at all: the direct equip paths run as without APMF. Logged once.
    bool EquipAuthoritySupported() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || !Config::g_apmfEquipAuthority.load()) return false;
        if (api->abiVersion < 9) {
            static std::atomic<bool> s_warnedEquipAbi{ false };
            if (!s_warnedEquipAbi.exchange(true))
                spdlog::warn("[equip-auth] APMF ABI v{} has no SetEquipScope (need >= 9) -- equip authority "
                             "OFF: no claim, no declaration; MFO keeps its own equips (degrade to NO "
                             "authority, never to a blanket lock -- a v9 declaration omits the hands it "
                             "does not hold and a v8 APMF would own them by default and freeze them).",
                             api->abiVersion);
            return false;
        }
        return true;
    }

    bool IsEquipAuthorityEnforced() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < 9) return false;
        return reinterpret_cast<const APMF_API::APMF_API_v8*>(api)->IsEquipAuthorityEnforced();
    }

    bool ClaimEquipAuthority(RE::FormID a_follower, bool* a_outFresh) {
        if (a_outFresh) *a_outFresh = false;
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || api->abiVersion < 9 || !Config::g_apmfEquipAuthority.load()) return false;
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        if (o.equipAuthHandle != APMF_API::kInvalidHandle) {
            // F2: standing means "kept", not "believed forever". Once the handle
            // is old enough to have been published, ask APMF whether it still
            // knows it; a dead one (released by APMF's hotkey/unload sweep, or a
            // New Game that reused the base FormIDs) is re-minted below.
            const auto now = std::chrono::steady_clock::now();
            if (now - o.equipAuthMintedAt < kEquipAuthValidateAfter ||
                reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->IsClaimLive(o.equipAuthHandle)) {
                if (a_outFresh) *a_outFresh = o.equipAuthFresh;
                return true;
            }
            spdlog::warn("[equip-auth] {:08X}: standing claim handle {} is no longer live on APMF's side "
                         "(released or swept there; SetEquipSet on it would be a silent no-op) -- re-minting",
                         a_follower, o.equipAuthHandle);
            o.equipAuthHandle = APMF_API::kInvalidHandle;   // no Release: APMF already forgot it
        }
        // param.ival = the EquipAuthFlags word. kEquipAuth_None on purpose:
        // scripts/console pass (a quest author's deliberate act), unequips are
        // never denied by this ABI, and observe-only is APMF's INI decision for
        // the probe cycle -- MFO does not set kEquipAuth_ObserveOnly itself, so
        // flipping APMF's bEquipObserveOnly=0 is the ONE switch that enforces.
        APMF_API::APMF_Param p{};
        p.ival = static_cast<std::int32_t>(APMF_API::kEquipAuth_None);
        o.equipAuthHandle = api->RequestEx(a_follower, APMF_API::kIntent_EquipAuthority, kOwnBasis, &p);
        if (o.equipAuthHandle == APMF_API::kInvalidHandle) {
            // APMF present at v7 and it REFUSED the claim (F1/F5, Fable round 2:
            // APMF is being changed to refuse exactly when its #17a equip seat is
            // NOT installed -- INI off, a site-verify refusal, VR). There is then
            // nothing on APMF's side that could perform or deny an equip, so
            // MFO's OWN equip paths are the right ones: every caller reads
            // `false` here (and IsEquipAuthorityClaimed false) as "the authority
            // is not live for this follower" and runs its direct equips exactly
            // as without APMF. Logged once per refusal streak so the field log
            // shows which followers are on which path.
            if (g_equipAuthRefused.insert(a_follower).second) {
                spdlog::warn("[equip-auth] {:08X}: APMF refused the kIntent_EquipAuthority claim "
                             "(RequestEx returned kInvalidHandle; APMF's equip seat is not installed "
                             "or the channel is unavailable) -- MFO keeps its own equips for this "
                             "follower (the direct paths run, as without APMF)",
                             a_follower);
            }
            EraseIfEmpty(g_owned.find(a_follower));
            return false;
        }
        g_equipAuthRefused.erase(a_follower);
        o.equipAuthMintedAt = std::chrono::steady_clock::now();
        o.equipAuthFresh    = true;
        o.equipOwned        = APMF_API::kEquipCat_All;   // APMF's default on a new claim (v9)
        o.equipDenied       = 0;
        if (a_outFresh) *a_outFresh = true;
        // mode= is APMF's v8 IsEquipAuthorityEnforced read at claim time (seat
        // installed AND bEquipObserveOnly=0), so a Deck log states observe vs
        // enforce in the line that opens each follower's authority.
        spdlog::info("[equip-auth] {:08X}: claim (kIntent_EquipAuthority, basis {}, flags 0, handle {}, mode={})",
                     a_follower, kOwnBasis, o.equipAuthHandle,
                     reinterpret_cast<const APMF_API::APMF_API_v8*>(api)->IsEquipAuthorityEnforced()
                         ? "ENFORCE" : "observe-only");
        return true;
    }

    void ReleaseEquipAuthority(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        if (it->second.equipAuthHandle != APMF_API::kInvalidHandle) {
            RE::FormID dummy = 0;
            ReleaseHandleLocked(it->second.equipAuthHandle, dummy);
            spdlog::info("[equip-auth] {:08X}: release", a_follower);
        }
        EraseIfEmpty(it);
    }

    bool IsEquipAuthorityClaimed(RE::FormID a_follower) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() && it->second.equipAuthHandle != APMF_API::kInvalidHandle;
    }

    bool DeclareEquipScope(RE::FormID a_follower, std::uint32_t a_owned, std::uint32_t a_denied) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || api->abiVersion < 9 || !Config::g_apmfEquipAuthority.load()) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        if (it == g_owned.end() || it->second.equipAuthHandle == APMF_API::kInvalidHandle) {
            spdlog::error("[equip-auth] {:08X}: DeclareEquipScope without a standing claim -- "
                          "ClaimEquipAuthority first (scope NOT sent)", a_follower);
            return false;
        }
        if ((a_owned | a_denied) & ~static_cast<std::uint32_t>(APMF_API::kEquipCat_All))
            spdlog::error("[equip-auth] {:08X}: scope carries bits outside kEquipCat_All (owned=0x{:X} "
                          "denied=0x{:X}) -- APMF masks them off (MFO's composition bug)",
                          a_follower, a_owned, a_denied);
        // COPIED inside the call (APMF_API.h: read synchronously, never retained);
        // a stack temporary is the documented shape. Sent BEFORE the set by the
        // one caller (RefreshEquipDeclaration): two enqueues, normally applied in
        // the same Drain; a Drain landing between them pairs the NEW scope with
        // the OLD set for one frame (self-healing; MFO-B45), never the reverse.
        APMF_API::APMF_EquipScope scope{};
        scope.owned  = a_owned;
        scope.denied = a_denied;
        reinterpret_cast<const APMF_API::APMF_API_v9*>(api)->SetEquipScope(it->second.equipAuthHandle, &scope);
        it->second.equipOwned  = a_owned;
        it->second.equipDenied = a_denied;
        // equipAuthFresh is cleared by DeclareEquipSet, which always follows: the
        // scope alone is not a declaration.
        return true;
    }

    bool EquipAuthorityOwns(RE::FormID a_follower, std::uint32_t a_categories) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        if (it == g_owned.end() || it->second.equipAuthHandle == APMF_API::kInvalidHandle) return false;
        if (it->second.equipAuthFresh) return false;   // no declaration out yet: APMF allows everything
        return (it->second.equipOwned & a_categories) != 0;
    }

    // The MIRROR of EquipAuthorityOwns, on the `denied` half of the same sent scope.
    // Same guards, same lock, same "no declaration out yet -> nothing is in effect".
    bool EquipAuthorityDenies(RE::FormID a_follower, std::uint32_t a_categories) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        if (it == g_owned.end() || it->second.equipAuthHandle == APMF_API::kInvalidHandle) return false;
        if (it->second.equipAuthFresh) return false;   // no declaration out yet: APMF denies nothing
        return (it->second.equipDenied & a_categories) != 0;
    }

    bool DeclareEquipSet(RE::FormID a_follower, const std::vector<APMF_API::APMF_EquipEntry>& a_forms) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || api->abiVersion < 9 || !Config::g_apmfEquipAuthority.load()) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        if (it == g_owned.end() || it->second.equipAuthHandle == APMF_API::kInvalidHandle) {
            spdlog::error("[equip-auth] {:08X}: DeclareEquipSet without a standing claim -- "
                          "ClaimEquipAuthority first (declaration NOT sent)", a_follower);
            return false;
        }
        if (a_forms.size() > APMF_API::kMaxEquipSet)
            spdlog::error("[equip-auth] {:08X}: declared set has {} items, over kMaxEquipSet {} -- "
                          "truncated; APMF treats the excess as off-set (MFO's composition bug)",
                          a_follower, a_forms.size(), APMF_API::kMaxEquipSet);
        const std::uint32_t count = a_forms.size() > APMF_API::kMaxEquipSet
                                      ? APMF_API::kMaxEquipSet
                                      : static_cast<std::uint32_t>(a_forms.size());
        // COPIED inside the call (APMF_API.h threading contract); a_forms may die.
        // v8 SetEquipSetEx: one entry per item WITH its hand (kEquipSlot_Right /
        // kEquipSlot_Left for the hand-held items MFO decides, Default for the
        // rest), so APMF itself places a dual-wielder's off-hand weapon in the
        // LEFT hand -- the slot-less v7 pass could not (Loadout.cpp F2).
        reinterpret_cast<const APMF_API::APMF_API_v8*>(api)->SetEquipSetEx(
            it->second.equipAuthHandle, count ? a_forms.data() : nullptr, count);
        it->second.equipAuthFresh = false;   // this handle now carries a declaration
        return true;
    }

}
