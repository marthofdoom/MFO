// apmf/Excursion.cpp -- the per-combat / per-excursion facet claims: the
// combat target, the ch.9 package offer, the ch.19 loot-travel legs and the
// combat-action deny.
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

    namespace {
        // ── ch.19 kIntent_Travel (test/mfo-loot-travel-via-ch19) ──────────────────
        // APMF_API::kIntent_Travel and APMF_API::kTravel_ReleaseOnTargetDead come
        // from the byte-shared header (re-mirrored at ABI v10). The runtime check
        // below stays as defence: the ABI that first implements ch.19.
        constexpr std::uint32_t    kTravelMinAbi = 10;

        // ONE ch.19 leg per MFO loot slot. Keyed by SLOT because that is the only key
        // every release edge has (Logistics.cpp's sweeps call
        // Packages::LootTravelClear with a null actor and a slot index).
        // NO EXPIRY SWEEP ENTRY, deliberately: unlike the ch.9 offer claim -- which
        // Packages::Pump() re-offers unconditionally every ~133 ms and which therefore
        // can afford kExpiry as a backstop -- nothing refreshes a travel claim, so an
        // expiry here would be a timer racing a live leg (CLAUDE.md principle 9). The
        // lifecycle is caller-driven and CLOSED: Packages::LootTravelClear releases
        // the slot at every end (combat, arrival, excursion cap, leash, player
        // combat, subsystem off, batch done, revert) and LootTravelEvictIf releases by
        // follower on dismissal and on a retreat preempt.
        struct LootTravelLeg {
            APMF_API::Handle handle   = APMF_API::kInvalidHandle;
            RE::FormID       follower = 0;
            RE::FormID       dest     = 0;
            float            radius   = 0.0f;
        };
        LootTravelLeg g_lootTravelLeg[Packages::kMaxLootSlots]{};   // guarded by g_mx

        // ival twin of EnsureClaimLocked, for kIntent_CombatAction's param.ival
        // (a category bitmask) rather than a param.form. Same create / re-point-in-
        // place (v3) / release+request (v2 fallback) shape; `want == 0` releases.
        void EnsureIvalClaimLocked(const APMF_API::APMF_API_v2* api, RE::FormID follower,
                                   APMF_API::Intent intent, APMF_API::Handle& handle,
                                   std::uint32_t& cur, std::uint32_t want) {
            if (want == 0) {
                if (handle != APMF_API::kInvalidHandle) { api->Release(handle); handle = APMF_API::kInvalidHandle; }
                cur = 0;
                return;
            }
            if (handle != APMF_API::kInvalidHandle && cur == want) return;   // unchanged
            APMF_API::APMF_Param p{};
            p.ival = static_cast<std::int32_t>(want);
            if (handle == APMF_API::kInvalidHandle) {
                handle = api->RequestEx(follower, intent, kOwnBasis, &p);
                cur    = (handle != APMF_API::kInvalidHandle) ? want : 0;
            } else if (api->abiVersion >= 3) {
                reinterpret_cast<const APMF_API::APMF_API_v3*>(api)->Repoint(handle, &p);
                cur = want;
            } else {
                api->Release(handle);
                handle = api->RequestEx(follower, intent, kOwnBasis, &p);
                cur    = (handle != APMF_API::kInvalidHandle) ? want : 0;
            }
        }

    }

    // ── combat-TARGET (per-combat) ──────────────────────────────────────────────
    void ClaimCombatTarget(RE::FormID a_follower, RE::FormID a_target, bool a_create) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_target == 0 || !Config::g_apmfCast.load()) return;
        std::scoped_lock lock(g_mx);
        Owned* o = nullptr;
        if (a_create) {
            o = &g_owned[a_follower];
        } else {
            // Refresh/re-point ONLY an existing claim -- never CREATE one from a
            // non-cast directive, so a pure-melee follower (who never cast) keeps
            // using MFO's own targeting, not APMF combat-target.
            auto it = g_owned.find(a_follower);
            if (it == g_owned.end() || it->second.targetHandle == APMF_API::kInvalidHandle) return;
            o = &it->second;
        }
        EnsureClaimLocked(api, a_follower, APMF_API::kIntent_CombatTarget, o->targetHandle, o->target, a_target);
        o->targetRefreshed = std::chrono::steady_clock::now();
        EraseIfEmpty(g_owned.find(a_follower));
    }

    void RefreshCombatTarget(RE::FormID a_follower) {
        if (!g_apmf.load(std::memory_order_relaxed)) return;
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it != g_owned.end() && it->second.targetHandle != APMF_API::kInvalidHandle)
            it->second.targetRefreshed = std::chrono::steady_clock::now();   // keep-alive: timestamp only
    }

    // ── ch.20 TARGET PIN (ABI v13, kIntent_TargetPin) ──────────────────────────────
    // Contract: APMF_API.h kIntent_TargetPin; Harbinger Docs/INTEGRATION.md "Pinning a
    // combat target". The CLIENT declares {follower, foe}; Harbinger answers the engine's
    // own target selector with it. MFO writes no target of its own on this route
    // (Targeting.cpp's hook stands down -- Targeting.h "the pin route").
    namespace {
        constexpr std::uint32_t kPinMinAbi = 13;   // the ABI that first serves intent 20

        // IsClaimLive reads APMF's PUBLISHED map, which a RequestEx reaches only at
        // APMF's next writer Drain. A pin MFO has never seen live is therefore not
        // "ended" merely because IsClaimLive is false yet -- only once it has been
        // seen live, or once this long has passed since it was filed. Same value and
        // same reasoning as kEquipAuthValidateAfter (ch.17's standing claim). A FLOOR,
        // not an expiry (CLAUDE.md principle 9): it only delays noticing an end.
        constexpr auto kPinValidateAfter = std::chrono::seconds(2);

        struct TargetPinClaim {
            APMF_API::Handle handle   = APMF_API::kInvalidHandle;   // invalid once ENDED
            RE::FormID       target   = 0;     // the foe MFO chose (kept after an end: the no-loop key)
            RE::ActorHandle  targetH{};        // same foe, as a handle (Targeting::Current)
            std::chrono::steady_clock::time_point filedAt{};
            bool everLive       = false;
            bool ended          = false;       // Harbinger ended it (IsClaimLive went false)
            bool suppressLogged = false;       // one [target-pin] line per ended pin, not per tick
        };
        std::unordered_map<RE::FormID, TargetPinClaim> g_pins;   // guarded by g_mx

        // Liveness of one filed pin. Marks it ENDED (and says so, once) when APMF no
        // longer holds it. Returns true iff the pin is ended. g_mx HELD.
        bool PinEndedLocked(const APMF_API::APMF_API_v2* api, RE::FormID follower, TargetPinClaim& p,
                            std::chrono::steady_clock::time_point now) {
            if (p.ended) return true;
            if (p.handle == APMF_API::kInvalidHandle) return false;
            // v6 IsClaimLive: this table only ever holds handles from an ABI >= 13 APMF.
            if (reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->IsClaimLive(p.handle)) {
                p.everLive = true;
                return false;
            }
            if (!p.everLive && now - p.filedAt < kPinValidateAfter) return false;   // not drained yet
            spdlog::info("[target-pin] {:08X}: Harbinger ENDED the pin on {:08X} (h={}; target lost, dead, "
                         "disabled, unloaded or unresolvable, the follower dead, or an outranking claim -- "
                         "APMF's '[ch.20] pin ended' line names it). The engine picks until MFO's gambit "
                         "chooses a foe again; {:08X} itself is not re-pinned until the choice moves.",
                         follower, p.target, p.handle, p.target);
            p.handle = APMF_API::kInvalidHandle;   // dead: Harbinger released it (Release would be a no-op)
            p.ended  = true;
            return true;
        }
    }

    bool TargetPinOffered() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= kPinMinAbi;
    }

    PinResult PinTarget(RE::FormID a_follower, RE::FormID a_target, RE::ActorHandle a_targetHandle) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kPinMinAbi) return PinResult::SeatAbsent;
        // APMF refuses these synchronously too; filtering them here keeps a synchronous
        // refusal meaning exactly ONE thing to Targeting (the seat is not installed).
        if (a_follower == 0 || a_target == 0 || a_target == a_follower || a_follower == 0x14)
            return PinResult::Invalid;

        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(g_mx);
        auto it = g_pins.find(a_follower);
        if (it != g_pins.end()) {
            auto& p = it->second;
            const bool ended = PinEndedLocked(api, a_follower, p, now);
            if (p.target == a_target) {
                if (!ended) return PinResult::Unchanged;
                // THE NO-LOOP RULE. Harbinger dropped this foe; MFO's selectors already
                // skip dead / disabled / lost foes, so reaching here means a foe they
                // still rate (e.g. an outranking claim, or unloaded 3D) -- re-pinning it
                // would be ended again at Harbinger's next poll, forever.
                if (!p.suppressLogged) {
                    p.suppressLogged = true;
                    spdlog::info("[target-pin] {:08X}: gambit re-chose {:08X} after Harbinger ended its pin -- "
                                 "NOT re-pinned (it pins again once the gambit chooses another foe first). "
                                 "(Logged once per ended pin.)",
                                 a_follower, a_target);
                }
                return PinResult::Suppressed;
            }
            // A CHANGED choice. Release + a fresh RequestEx rather than Repoint: a
            // Repoint onto a handle Harbinger ended a moment ago is silently a no-op,
            // and the next sweep would then record the NEW foe as the one Harbinger
            // dropped and suppress it. Both ops are queued in order and applied at
            // APMF's same writer Drain, so there is no unpinned gap.
            if (p.handle != APMF_API::kInvalidHandle) api->Release(p.handle);
            g_pins.erase(it);
        }

        APMF_API::APMF_Param prm{};
        prm.form = a_target;   // REQUIRED: the target ACTOR. Nothing else is read.
        const APMF_API::Handle h = api->RequestEx(a_follower, APMF_API::kIntent_TargetPin, kOwnBasis, &prm);
        if (h == APMF_API::kInvalidHandle) {
            // Synchronous refusal with valid params = the ch.20 seat is not installed
            // (VR, runtime, [TargetPin] bTargetPin=0, self-check). Session-stable: the
            // seat installs once at APMF's kDataLoaded. Targeting logs the route change.
            return PinResult::SeatAbsent;
        }
        TargetPinClaim p{};
        p.handle  = h;
        p.target  = a_target;
        p.targetH = a_targetHandle;
        p.filedAt = now;
        g_pins.emplace(a_follower, p);
        spdlog::info("[target-pin] {:08X}: PIN {:08X} (h={})", a_follower, a_target, h);
        return PinResult::Pinned;
    }

    void ReleaseTargetPin(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_mx);
        auto it = g_pins.find(a_follower);
        if (it == g_pins.end()) return;
        if (api && it->second.handle != APMF_API::kInvalidHandle) api->Release(it->second.handle);
        spdlog::info("[target-pin] {:08X}: release (target {:08X}{})", a_follower, it->second.target,
                     it->second.ended ? ", already ended by Harbinger" : "");
        g_pins.erase(it);
    }

    void ReleaseAllTargetPins() {
        std::scoped_lock lock(g_mx);
        ClearTargetPinsLocked();
    }

    RE::ActorHandle PinnedTarget(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kPinMinAbi) return {};
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(g_mx);
        auto it = g_pins.find(a_follower);
        if (it == g_pins.end()) return {};
        if (PinEndedLocked(api, a_follower, it->second, now)) return {};
        return it->second.targetH;
    }

    void SweepTargetPinsLocked(const APMF_API::APMF_API_v2* api, std::chrono::steady_clock::time_point now) {
        if (g_pins.empty() || !api || api->abiVersion < kPinMinAbi) return;
        // KILL SWITCH: bCommandTarget flipped OFF mid-session releases every pin on the
        // pump's cadence, as the legacy hook stopped writing the moment it read the flag.
        if (!Config::g_commandTarget.load()) {
            spdlog::info("[target-pin] bCommandTarget off -- releasing {} pin(s)", g_pins.size());
            ClearTargetPinsLocked();
            return;
        }
        for (auto& [fid, p] : g_pins) PinEndedLocked(api, fid, p, now);
    }

    void ClearTargetPinsLocked() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        for (auto& [fid, p] : g_pins)
            if (api && p.handle != APMF_API::kInvalidHandle) api->Release(p.handle);
        g_pins.clear();
    }

    // ── package-offer (per-excursion) ───────────────────────────────────────────
    bool OfferPackage(RE::FormID a_follower, RE::FormID a_packageForm) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_packageForm == 0 || !Config::g_apmfLootTravel.load()) return false;
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureClaimLocked(api, a_follower, APMF_API::kIntent_OfferPackage, o.packageHandle, o.package, a_packageForm);
        o.packageRefreshed = std::chrono::steady_clock::now();
        const bool live = o.packageHandle != APMF_API::kInvalidHandle;
        EraseIfEmpty(g_owned.find(a_follower));
        return live;
    }

    void ReleaseOfferPackage(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseHandleLocked(it->second.packageHandle, it->second.package);
        EraseIfEmpty(it);
    }

    // -- ch.19 TRAVEL facet: loot-travel ROAD 2 (A/B, see APMFBridge.h) ----------
    bool ClaimLootTravel(RE::FormID a_follower, RE::FormID a_destRef, int a_slot, float a_radius) {
        if (a_slot < 0 || a_slot >= Packages::kMaxLootSlots) return false;
        if (a_follower == 0 || a_destRef == 0) return false;
        auto* api = g_apmf.load(std::memory_order_relaxed);

        std::scoped_lock lock(g_mx);
        auto& leg = g_lootTravelLeg[a_slot];
        // THE SWITCH IS READ ONLY WHEN THERE IS NO LEG YET -- never on the re-point
        // path below. That is what makes flipping bLootTravelViaApmfTravel mid-session
        // safe: a leg already in flight on this road keeps being re-pointed (and
        // released) on this road however the switch now reads, and the switch decides
        // only which road the NEXT excursion's first dispatch takes. It is also read
        // AHEAD of the ABI report below so a control-road session with no APMF never
        // logs a ch.19 line at all.
        const bool haveLeg = leg.handle != APMF_API::kInvalidHandle;
        if (!haveLeg && !Config::g_lootTravelViaApmfTravel.load()) return false;

        // REFUSAL, class 1: no APMF, or an APMF older than ch.19. Named ONCE per
        // class (not per dispatch): the caller takes the ch.9 control road, and a
        // per-dispatch line would drown the road comparison this branch exists for.
        if (!api || api->abiVersion < kTravelMinAbi) {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true)) {
                if (api)
                    spdlog::warn("[loot-road] ch.19 travel UNAVAILABLE -- APMF is ABI v{}, ch.19 "
                                 "kIntent_Travel needs v{} -- every loot dispatch takes the ch.9 "
                                 "MFO-package road instead. (Logged once.)",
                                 api->abiVersion, kTravelMinAbi);
                else
                    spdlog::warn("[loot-road] ch.19 travel UNAVAILABLE -- APMF.dll is not present -- "
                                 "every loot dispatch takes the ch.9 MFO-package road instead. "
                                 "(Logged once.)");
            }
            return false;
        }

        APMF_API::APMF_Param p{};
        p.form = a_destRef;                            // REQUIRED: the destination REFERENCE
        p.fval = a_radius;                             // arrival radius; APMF clamps to [50,512]
        p.ival = static_cast<std::int32_t>(APMF_API::kTravel_ReleaseOnTargetDead);   // names ch.19's v1 default
        // posX/Y/Z stay ZERO -- a non-zero position REFUSES the claim (a package
        // location carries a form or a handle, never coordinates).

        if (haveLeg) {
            // RETARGET: the same slot, a new destination -> RE-POINT in place. APMF
            // rewrites its package's Location and re-files/re-points its own internal
            // ch.9 offer so the nudge fires again; a leg that had already ENDED (it
            // arrived, or APMF abandoned it) is STARTED FRESH by the same call. No
            // release/re-claim churn either way. Repoint is an ABI v3 slot and this
            // claim cannot exist below v10, so the v3 cast is unconditional here.
            if (leg.dest == a_destRef && leg.radius == a_radius) return true;   // cheap no-op
            reinterpret_cast<const APMF_API::APMF_API_v3*>(api)->Repoint(leg.handle, &p);
            leg.dest   = a_destRef;
            leg.radius = a_radius;
            spdlog::info("[loot-road] {:08X}: RETARGET road=CH19 dest={:08X} slot={} radius={:.0f} "
                         "handle={} (re-pointed in place)",
                         a_follower, a_destRef, a_slot, a_radius, leg.handle);
            return true;
        }

        const APMF_API::Handle h = api->RequestEx(a_follower, APMF_API::kIntent_Travel, kOwnBasis, &p);
        if (h == APMF_API::kInvalidHandle) {
            // REFUSAL, class 2: a SYNCHRONOUS refusal from a live, v10+ APMF --
            // Data/APMF.esl missing or disabled, [Travel] bTravel=0, VR, or all EIGHT
            // of APMF's travel slots busy. APMF names the reason in ITS log. Never
            // throttled: unlike class 1 this can come and go within a session (the
            // slot cap), and the caller's fallback to the ch.9 road is exactly the
            // thing marth is A/B-ing, so every occurrence is reported.
            spdlog::warn("[loot-road] {:08X}: ch.19 travel claim REFUSED (dest={:08X}, slot={}, "
                         "radius={:.0f}) -- APMF's own log names the reason (esl absent, [Travel] "
                         "bTravel=0, VR, or all 8 travel slots busy). Falling back to the ch.9 "
                         "MFO-package road for THIS excursion.",
                         a_follower, a_destRef, a_slot, a_radius);
            return false;
        }
        leg.handle   = h;
        leg.follower = a_follower;
        leg.dest     = a_destRef;
        leg.radius   = a_radius;
        spdlog::info("[loot-road] {:08X}: DISPATCH road=CH19 dest={:08X} slot={} radius={:.0f} handle={}",
                     a_follower, a_destRef, a_slot, a_radius, h);
        return true;
    }

    bool HasLootTravelLeg(int a_slot) {
        if (a_slot < 0 || a_slot >= Packages::kMaxLootSlots) return false;
        std::scoped_lock lock(g_mx);
        return g_lootTravelLeg[a_slot].handle != APMF_API::kInvalidHandle;
    }

    bool ReleaseLootTravelSlot(int a_slot) {
        if (a_slot < 0 || a_slot >= Packages::kMaxLootSlots) return false;
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_mx);
        auto& leg = g_lootTravelLeg[a_slot];
        if (leg.handle == APMF_API::kInvalidHandle) return false;   // the ch.9 control road
        // MANDATORY even when APMF already ended the leg itself: APMF never revokes a
        // claim the client still holds, so an unreleased handle keeps an APMF travel
        // slot's claim alive (1 of only 8) doing nothing.
        if (api) api->Release(leg.handle);   // the caller (Packages::LootTravelClear) logs the [loot-road] line
        leg = LootTravelLeg{};
        return true;
    }

    int ReleaseLootTravelFor(RE::FormID a_follower) {
        if (a_follower == 0) return 0;
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_mx);
        int n = 0;
        for (int i = 0; i < Packages::kMaxLootSlots; ++i) {
            auto& leg = g_lootTravelLeg[i];
            if (leg.handle == APMF_API::kInvalidHandle || leg.follower != a_follower) continue;
            if (api) api->Release(leg.handle);
            leg = LootTravelLeg{};
            ++n;
        }
        return n;
    }

    // ── combat-action deny (per-excursion) ──────────────────────────────────────
    bool ClaimCombatActionDeny(RE::FormID a_follower, std::uint32_t a_categoryMask) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_categoryMask == 0 || !Config::g_apmfLootTravel.load()) return false;
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureIvalClaimLocked(api, a_follower, APMF_API::kIntent_CombatAction, o.actionHandle, o.actionMask, a_categoryMask);
        o.actionRefreshed = std::chrono::steady_clock::now();
        const bool live = o.actionHandle != APMF_API::kInvalidHandle;
        EraseIfEmpty(g_owned.find(a_follower));
        return live;
    }

    void ReleaseCombatActionDeny(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseHandleLocked(it->second.actionHandle, it->second.actionMask);
        EraseIfEmpty(it);
    }

}
