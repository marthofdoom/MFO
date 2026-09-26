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
            // loot M2 (ABI v12): the leg-state seq read just BEFORE our latest
            // RequestEx/Repoint. A state still carrying it is the previous leg's
            // (APMF applies the call on its next Drain), never this leg's end.
            std::uint32_t    staleSeq = 0;
            bool             hasStale = false;
        };
        LootTravelLeg g_lootTravelLeg[Packages::kMaxLootSlots]{};   // guarded by g_mx

        // ── ch.19 LEG STATE (loot M2, ABI v12) ──────────────────────────────────
        // The first ABI with APMF_API_v12::GetTravelLegState, the BLOCKED end and
        // the gait bits. Below it APMF stores unknown TravelFlags bits without a
        // word, so the gait bits are never sent there (a masked no-op otherwise).
        constexpr std::uint32_t kTravelLegStateAbi = 12;

        const APMF_API::APMF_API_v12* LegStateApi(const APMF_API::APMF_API_v2* a_api) {
            if (!a_api || a_api->abiVersion < kTravelLegStateAbi) return nullptr;
            auto* v12 = reinterpret_cast<const APMF_API::APMF_API_v12*>(a_api);
            return v12->GetTravelLegState ? v12 : nullptr;
        }

        // One snapshot read; false when there is no v12 slot. The caller's size is
        // sizeof(APMF_TravelLegInfo) = 72 = kTravelLegInfoV12Size (the header's own
        // static_asserts pin both), so APMF fills every field this build knows.
        bool ReadLegInfo(const APMF_API::APMF_API_v2* a_api, RE::FormID a_actor,
                         APMF_API::APMF_TravelLegInfo& a_info) {
            static_assert(sizeof(APMF_API::APMF_TravelLegInfo) >= APMF_API::kTravelLegInfoV12Size,
                          "loot M2 reads the v12 leg-state prefix");
            auto* v12 = LegStateApi(a_api);
            if (!v12 || a_actor == 0) return false;
            a_info      = APMF_API::APMF_TravelLegInfo{};
            a_info.size = static_cast<std::uint32_t>(sizeof(APMF_API::APMF_TravelLegInfo));
            v12->GetTravelLegState(a_actor, &a_info);
            return true;
        }

        // An ENDED leg: APMF dropped its package offer and the claim does nothing
        // until it is re-pointed or released.
        bool LegEnded(std::uint32_t a_state) {
            return a_state >= APMF_API::kLeg_Arrived && a_state <= APMF_API::kLeg_Released;
        }

        // Record the seq that stands BEFORE a RequestEx/Repoint on `a_leg`.
        // LOCK ORDER: called with g_mx HELD, and it calls APMF's GetTravelLegState
        // under it. A safe LEAF edge: APMF takes only its own per-actor mirror mutex
        // for the copy and never calls back into MFO (the same shape as the
        // RequestEx/Repoint/Release calls this file already makes under g_mx).
        void NoteStaleSeqLocked(const APMF_API::APMF_API_v2* a_api, RE::FormID a_actor, LootTravelLeg& a_leg) {
            APMF_API::APMF_TravelLegInfo info{};
            a_leg.hasStale = ReadLegInfo(a_api, a_actor, info);
            a_leg.staleSeq = a_leg.hasStale ? info.seq : 0;
        }

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
        // APMF's next writer Drain -- and APMF drains ONLY on its PlayerCharacter::Update
        // seat, which does not tick while a game-pausing menu is open. So a pin MFO has
        // never seen live is judged "ended" only after this many UNPAUSED sweeps (the
        // bridge pump, ~133 ms each: ~2 s of running game), never on wall time. A FLOOR,
        // not an expiry (CLAUDE.md principle 9): it only delays noticing an end.
        constexpr std::uint32_t kPinNeverLiveSweeps = 15;

        struct TargetPinClaim {
            APMF_API::Handle handle   = APMF_API::kInvalidHandle;   // invalid once ENDED
            RE::FormID       target   = 0;     // the foe MFO chose (kept after an end: the no-loop key)
            RE::ActorHandle  targetH{};        // same foe, as a handle (Targeting::Current)
            std::uint32_t    unpausedSweeps = 0;   // sweeps with the game running since filing
            bool everLive       = false;       // seen IsClaimLive at least once (kept after an end)
            bool ended          = false;       // Harbinger ended it (IsClaimLive went false)
            bool suppressLogged = false;       // one [target-pin] line per ended pin, not per tick
        };
        std::unordered_map<RE::FormID, TargetPinClaim> g_pins;   // guarded by g_mx

        bool GamePaused() {
            auto* ui = RE::UI::GetSingleton();   // same read Scheduler.cpp's tick gate makes on this worker
            return ui && ui->GameIsPaused();
        }

        // Liveness of one filed pin. Marks it ENDED (and says so, once) when APMF no
        // longer holds it. Returns true iff the pin is ended. g_mx HELD.
        bool PinEndedLocked(const APMF_API::APMF_API_v2* api, RE::FormID follower, TargetPinClaim& p) {
            if (p.ended) return true;
            if (p.handle == APMF_API::kInvalidHandle) return false;
            // v6 IsClaimLive: this table only ever holds handles from an ABI >= 13 APMF.
            if (reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->IsClaimLive(p.handle)) {
                p.everLive = true;
                return false;
            }
            if (!p.everLive) {
                // Never seen live: judge only with the game running, and only after the
                // floor of unpaused sweeps (APMF may simply not have drained yet).
                if (GamePaused() || p.unpausedSweeps < kPinNeverLiveSweeps) return false;
            }
            spdlog::info("[target-pin] {:08X}: pin on {:08X} ENDED (h={}, {}; Harbinger ends a pin when the "
                         "target is lost, dead, disabled, unloaded or unresolvable, or the follower dies -- "
                         "APMF's '[ch.20] pin ended' line names it). The engine picks until MFO's gambit "
                         "chooses a foe again.",
                         follower, p.target, p.handle, p.everLive ? "was live" : "never seen live");
            // ALWAYS Release before dropping the handle: a no-op on a claim Harbinger
            // already released, and it FIFO-cancels one still pending in APMF's queue,
            // so a false "never live" judgement can never leave an ORPHAN claim that
            // keeps winning on equal basis with no MFO handle left to release it.
            api->Release(p.handle);
            p.handle = APMF_API::kInvalidHandle;
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

        std::scoped_lock lock(g_mx);
        auto it = g_pins.find(a_follower);
        if (it != g_pins.end()) {
            auto& p = it->second;
            const bool ended = PinEndedLocked(api, a_follower, p);
            if (p.target == a_target) {
                if (!ended) return PinResult::Unchanged;
                // THE NO-LOOP RULE. The gambit re-chose the foe of an ENDED pin. MFO's foe
                // selector (Evaluator.cpp) skips exactly what ends a pin -- dead,
                // disabled, lost (kTargetLost), 3D not loaded -- so being chosen again IS
                // MFO observing the foe trackable again. Re-pin it IF the ended pin had
                // gone live (Harbinger applied it, then lost track: the foe was briefly
                // lost and is back). A pin that NEVER went live is not re-pinned: nothing
                // MFO can see explains why it never took, so re-filing it would repeat.
                if (!p.everLive) {
                    if (!p.suppressLogged) {
                        p.suppressLogged = true;
                        spdlog::info("[target-pin] {:08X}: gambit re-chose {:08X}, whose pin ended without ever "
                                     "going live -- NOT re-pinned (it pins again once the gambit chooses another "
                                     "foe first). (Logged once per ended pin.)",
                                     a_follower, a_target);
                    }
                    return PinResult::Suppressed;
                }
                spdlog::info("[target-pin] {:08X}: {:08X} is trackable again (re-chosen by the gambit) -- re-pinning",
                             a_follower, a_target);
            }
            // A CHANGED choice (or the re-pin above). Release + a fresh RequestEx rather than Repoint: a
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
        std::scoped_lock lock(g_mx);
        auto it = g_pins.find(a_follower);
        if (it == g_pins.end()) return {};
        if (PinEndedLocked(api, a_follower, it->second)) return {};
        return it->second.targetH;
    }

    std::size_t TargetPinCount() {
        std::scoped_lock lock(g_mx);
        std::size_t n = 0;
        for (const auto& [fid, p] : g_pins) if (!p.ended) ++n;
        return n;
    }

    void SweepTargetPinsLocked(const APMF_API::APMF_API_v2* api, std::chrono::steady_clock::time_point /*now*/) {
        if (g_pins.empty() || !api || api->abiVersion < kPinMinAbi) return;
        // KILL SWITCH: bCommandTarget flipped OFF mid-session releases every pin on the
        // pump's cadence, as the legacy hook stopped writing the moment it read the flag.
        if (!Config::g_commandTarget.load()) {
            spdlog::info("[target-pin] bCommandTarget off -- releasing {} pin(s)", g_pins.size());
            ClearTargetPinsLocked();
            return;
        }
        // The never-live floor counts UNPAUSED sweeps only (APMF cannot drain while a
        // game-pausing menu is open), so a menu can never age a pending pin into "ended".
        const bool paused = GamePaused();
        for (auto& [fid, p] : g_pins) {
            if (!paused && !p.ended && !p.everLive) ++p.unpausedSweeps;
            PinEndedLocked(api, fid, p);
        }
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
        std::uint32_t flags = APMF_API::kTravel_ReleaseOnTargetDead;   // names ch.19's v1 default
        // GAIT (loot M2, 86e3dh44v): the walk-to-loot gait setting (iTravelGait,
        // clamped 0..3 at parse = the engine's PreferredSpeed Walk/Jog/Run/FastWalk)
        // as ch.19's speed bits, so APMF writes it into ITS leg package. ONLY at
        // ABI >= 12: an older APMF stores the bits and runs at its authored speed
        // without a word, which would be a silent no-op.
        const bool gaitBits = LegStateApi(api) != nullptr;
        const int  gait     = std::clamp(Config::g_travelGait.load(), 0, 3);
        if (gaitBits)
            flags |= APMF_API::kTravel_SpeedSet |
                     ((static_cast<std::uint32_t>(gait) << 3) & APMF_API::kTravel_SpeedMask);
        p.ival = static_cast<std::int32_t>(flags);
        // posX/Y/Z stay ZERO -- a non-zero position REFUSES the claim (a package
        // location carries a form or a handle, never coordinates).

        if (haveLeg) {
            // RETARGET: the same slot, a new destination -> RE-POINT in place. APMF
            // rewrites its package's Location and re-files/re-points its own internal
            // ch.9 offer so the nudge fires again; a leg that had already ENDED (it
            // arrived, or APMF abandoned it) is STARTED FRESH by the same call. No
            // release/re-claim churn either way. Repoint is an ABI v3 slot and this
            // claim cannot exist below v10, so the v3 cast is unconditional here.
            //
            // loot M2: the SAME destination is a cheap no-op only while the leg is
            // still running. If APMF already ENDED it (arrived, blocked, ...), a
            // re-dispatch to the same ref (a deferred item's turn, a dibs revisit)
            // must re-point to start a fresh leg, or the follower never walks and
            // the ended state is re-read forever. Without the v12 read (APMF < 12)
            // the no-op stays as it was.
            bool sameEnded = false;
            if (leg.dest == a_destRef && leg.radius == a_radius) {
                APMF_API::APMF_TravelLegInfo info{};
                sameEnded = ReadLegInfo(api, leg.follower, info) && LegEnded(info.state) &&
                            info.destForm == a_destRef &&
                            (info.ownerHandle == leg.handle || info.ownerHandle == APMF_API::kInvalidHandle);
                if (!sameEnded) return true;   // cheap no-op
            }
            NoteStaleSeqLocked(api, leg.follower, leg);
            reinterpret_cast<const APMF_API::APMF_API_v3*>(api)->Repoint(leg.handle, &p);
            leg.dest   = a_destRef;
            leg.radius = a_radius;
            spdlog::info("[loot-road] {:08X}: RETARGET road=CH19 dest={:08X} slot={} radius={:.0f} "
                         "handle={} gait={} (re-pointed in place{})",
                         a_follower, a_destRef, a_slot, a_radius, leg.handle,
                         gaitBits ? gait : -1, sameEnded ? "; same ref, its last leg had ended" : "");
            return true;
        }

        LootTravelLeg fresh{};
        NoteStaleSeqLocked(api, a_follower, fresh);

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
        leg.staleSeq = fresh.staleSeq;
        leg.hasStale = fresh.hasStale;
        spdlog::info("[loot-road] {:08X}: DISPATCH road=CH19 dest={:08X} slot={} radius={:.0f} handle={} "
                     "gait={}{}",
                     a_follower, a_destRef, a_slot, a_radius, h, gaitBits ? gait : -1,
                     gaitBits ? "" : " (APMF < v12: no gait bits, APMF's authored speed)");
        return true;
    }

    bool ReadLootTravelLeg(int a_slot, LootLegState& a_out) {
        a_out = LootLegState{};
        if (a_slot < 0 || a_slot >= Packages::kMaxLootSlots) return false;
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!LegStateApi(api)) return false;
        LootTravelLeg leg;
        {
            std::scoped_lock lock(g_mx);
            leg = g_lootTravelLeg[a_slot];
        }
        if (leg.handle == APMF_API::kInvalidHandle) return false;
        APMF_API::APMF_TravelLegInfo info{};
        if (!ReadLegInfo(api, leg.follower, info)) return false;
        a_out.state       = info.state;
        a_out.seq         = info.seq;
        a_out.msInState   = info.msInState;
        a_out.blockedMs   = info.blockedMs;
        a_out.speed       = info.speed;
        a_out.blockerKind = info.blockerKind;
        a_out.blocker     = info.blocker;
        a_out.dest        = info.destForm;
        a_out.stall       = RE::NiPoint3{ info.stallX, info.stallY, info.stallZ };
        a_out.ours = info.destForm == leg.dest &&
                     (info.ownerHandle == leg.handle || info.ownerHandle == APMF_API::kInvalidHandle) &&
                     !(leg.hasStale && info.seq == leg.staleSeq);
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

    // ── LOCKPICK HOLD (LP-M1, ClickUp 86e3edgha): ch.1 stand-still + ch.12 Idle v2 ──
    // Contract: APMF_API.h kIntent_Idle (v17 param) / kIntent_MovementBlock; Harbinger
    // Docs/INTEGRATION.md "Playing an idle at a target". The caller (logistics/Lockpick.cpp)
    // owns the window: it files the hold at the start of a pick, Repoints the idle once per
    // simulated pick break, and releases at the window's end. Nothing here re-asserts.
    namespace {
        constexpr std::uint32_t kIdleV2MinAbi = 17;   // the ABI that gives ch.12 a form + target

        // PENDING vs ENDED (INTEGRATION.md "When a claim is refused -- and the two timings"):
        // IsClaimLive reads APMF's PUBLISHED map, which a RequestEx reaches only at APMF's next
        // writer Drain (not while a menu pauses the game). A never-seen-live idle claim is
        // judged ended only after this many UNPAUSED polls AND this much wall time since the
        // filing. FLOORS, not expiries (principle 9): they only delay noticing an end. Same
        // values and reasoning as the ch.21 entry table (apmf/CombatEntry.cpp).
        constexpr std::uint32_t kPickNeverLivePolls = 15;
        constexpr auto          kPickNeverLiveFloor = std::chrono::seconds(2);

        struct LockpickHold {
            APMF_API::Handle idle  = APMF_API::kInvalidHandle;   // ch.12 v2 (invalid once ENDED)
            APMF_API::Handle block = APMF_API::kInvalidHandle;   // ch.1 stand-still
            RE::FormID       idleForm = 0;
            RE::FormID       lockRef  = 0;
            std::chrono::steady_clock::time_point filedAt{};
            std::uint32_t    unpausedPolls = 0;
            bool             everLive = false;
            bool             ended    = false;
        };
        std::unordered_map<RE::FormID, LockpickHold> g_pickHolds;   // guarded by g_mx

        // A synchronous refusal of a well-formed v2 idle = the v2 seat is not available
        // this session (VR, runtime, [Idle] bIdleV2=0, the SetupSpecialIdle self-check,
        // before kDataLoaded). Session-stable, never reset. Atomic: read without g_mx.
        std::atomic<bool> g_idleV2Refused{ false };

        bool PickGamePaused() {
            auto* ui = RE::UI::GetSingleton();   // the same read the entry / pin tables make on this worker
            return ui && ui->GameIsPaused();
        }

        void ReleasePickHoldLocked(const APMF_API::APMF_API_v2* api, LockpickHold& h) {
            if (api) {
                // Idle first: its Release resets a still-HELD idle to the default state
                // while the stand-still still holds him; then the stand-still.
                if (h.idle  != APMF_API::kInvalidHandle) api->Release(h.idle);
                if (h.block != APMF_API::kInvalidHandle) api->Release(h.block);
            }
            h.idle  = APMF_API::kInvalidHandle;
            h.block = APMF_API::kInvalidHandle;
        }
    }

    bool LockpickIdleOffered() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= kIdleV2MinAbi && !g_idleV2Refused.load(std::memory_order_relaxed);
    }

    PickHoldResult ClaimLockpickHold(RE::FormID a_follower, RE::FormID a_idle, RE::FormID a_lockRef) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kIdleV2MinAbi || g_idleV2Refused.load(std::memory_order_relaxed))
            return PickHoldResult::SeatAbsent;
        // Harbinger refuses target == actor synchronously too; filtering it here keeps a
        // synchronous refusal meaning exactly ONE thing (the v2 seat is not available).
        if (a_follower == 0 || a_idle == 0 || a_lockRef == 0 || a_lockRef == a_follower)
            return PickHoldResult::Invalid;

        std::scoped_lock lock(g_mx);
        if (g_pickHolds.contains(a_follower)) return PickHoldResult::Standing;

        APMF_API::APMF_Param ip{};
        ip.form   = a_idle;      // the IDLE record (v17: read only at abiVersion >= 17, checked above)
        ip.target = a_lockRef;   // play it AT the lock
        const APMF_API::Handle hi = api->RequestEx(a_follower, APMF_API::kIntent_Idle, kOwnBasis, &ip);
        if (hi == APMF_API::kInvalidHandle) {
            if (!g_idleV2Refused.exchange(true)) {
                spdlog::warn("[lockpick] Harbinger REFUSED the ch.12 idle v2 claim ({:08X}, idle {:08X} at "
                             "{:08X}) at the call: the v2 idle is not available (VR, a runtime other than "
                             "1.6.1170 / 1.5.97, [Idle] bIdleV2=0 in APMF.ini, or its address self-check; "
                             "APMF's log names it). Follower lockpicking is INERT for this session. MFO has "
                             "no direct road.",
                             a_follower, a_idle, a_lockRef);
            }
            return PickHoldResult::SeatAbsent;
        }
        // ch.1 has no param. A refusal here is not fatal to the pick (the idle still plays,
        // the follow package may walk him a step): logged, the hold goes on without it.
        const APMF_API::Handle hb = api->RequestEx(a_follower, APMF_API::kIntent_MovementBlock, kOwnBasis, nullptr);
        if (hb == APMF_API::kInvalidHandle)
            spdlog::warn("[lockpick] {:08X}: Harbinger refused the ch.1 stand-still for the pick window at "
                         "{:08X} (APMF's log names why); the idle plays without it", a_follower, a_lockRef);
        LockpickHold h{};
        h.idle     = hi;
        h.block    = hb;
        h.idleForm = a_idle;
        h.lockRef  = a_lockRef;
        h.filedAt  = std::chrono::steady_clock::now();
        g_pickHolds.emplace(a_follower, h);
        return PickHoldResult::Filed;
    }

    bool ReplayLockpickIdle(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kIdleV2MinAbi) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_pickHolds.find(a_follower);
        if (it == g_pickHolds.end() || it->second.ended || it->second.idle == APMF_API::kInvalidHandle)
            return false;
        APMF_API::APMF_Param ip{};
        ip.form   = it->second.idleForm;
        ip.target = it->second.lockRef;
        // v3 Repoint (every ABI >= 17 has it): a new declaration = one more PlayIdle.
        reinterpret_cast<const APMF_API::APMF_API_v3*>(api)->Repoint(it->second.idle, &ip);
        return true;
    }

    PickHoldState LockpickHoldStateOf(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_mx);
        const auto it = g_pickHolds.find(a_follower);
        if (it == g_pickHolds.end()) return PickHoldState::None;
        auto& h = it->second;
        if (h.ended) return PickHoldState::Ended;
        if (!api || h.idle == APMF_API::kInvalidHandle) {
            h.ended = true;
            return PickHoldState::Ended;
        }
        // v6 IsClaimLive: this table only ever holds handles from an ABI >= 17 APMF.
        if (reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->IsClaimLive(h.idle)) {
            h.everLive = true;
            return PickHoldState::Live;
        }
        if (!h.everLive) {
            if (!PickGamePaused()) ++h.unpausedPolls;
            if (h.unpausedPolls < kPickNeverLivePolls ||
                std::chrono::steady_clock::now() - h.filedAt < kPickNeverLiveFloor)
                return PickHoldState::Pending;
        }
        spdlog::info("[lockpick] {:08X}: the ch.12 idle claim at {:08X} ENDED on Harbinger's side (h={}, {}). "
                     "Harbinger ends it when the idle could not be played or the engine refused it, or the "
                     "follower died / unloaded; APMF's '[ch.12] ... idle claim ended' line names the reason.",
                     a_follower, h.lockRef, h.idle, h.everLive ? "was live" : "never seen live");
        // ALWAYS Release before dropping the handles: a no-op on a claim Harbinger already
        // released, and it FIFO-cancels one still pending, so a false "never live" judgement
        // can never leave an orphan claim. The stand-still goes with it.
        ReleasePickHoldLocked(api, h);
        h.ended = true;
        return PickHoldState::Ended;
    }

    void ReleaseLockpickHold(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_mx);
        const auto it = g_pickHolds.find(a_follower);
        if (it == g_pickHolds.end()) return;
        ReleasePickHoldLocked(api, it->second);
        g_pickHolds.erase(it);
    }

    void ReleaseAllLockpickHolds() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_mx);
        for (auto& [fid, h] : g_pickHolds) ReleasePickHoldLocked(api, h);
        g_pickHolds.clear();
    }

}
