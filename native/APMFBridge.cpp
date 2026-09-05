#include "APMFBridge.h"
#include "APMF_API.h"
#include "Config.h"
#include "Followers.h"   // g_active.size() -- round-robin-aware expiry sizing (FacetExpiry)
#include "MainThread.h"
#include "Rapport.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

// Two Win32 symbols, declared by hand. <windows.h> is BANNED outside Board.cpp --
// it #defines GetObject and hijacks BGSDefaultObjectManager::GetObject<T>
// (ENGINE_NOTES §9; the same reason Targeting.cpp / Logistics.cpp hand-declare
// GetModuleHandleA). Pointer args/return are pointer-sized on Win64, so void* is
// ABI-correct for HMODULE/FARPROC; the real header is never in this TU to conflict.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char* a_name);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void* a_module, const char* a_procName);

namespace MFO::APMFBridge {

    namespace {
        // The APMF interface (v2+, so RequestEx is present; Repoint needs v3 -- gated
        // per call on api->abiVersion). Written ONCE at kDataLoaded (main) before any
        // worker tick runs; read on the worker. Atomic (relaxed) enforces the
        // publish/consume contract cheaply.
        std::atomic<const APMF_API::APMF_API_v2*> g_apmf{ nullptr };

        // Per-follower owned-cast claims -- TWO INDEPENDENT LIFECYCLES:
        //   * cast-SELECT (spell): PER-CAST. Refreshed each winning cast tick; released
        //     CRISPLY the moment no cast rule holds (ReleaseCasting <- Scheduler
        //     !castSeen); the expiry is only a backstop. IMPORTANT (2026-09-05,
        //     corrects an earlier false assumption): "each winning cast tick" is a
        //     Scheduler::Tick ROUND-ROBIN lap for this follower's OWN service, not a
        //     tight combat-thread beat -- ONE follower is serviced per ~133ms pump
        //     (Scheduler.cpp), so the real refresh gap is ~0.133s * partySize. See
        //     FacetExpiry() below, which this claim now uses instead of the flat
        //     kExpiry.
        //   * combat-TARGET: PER-COMBAT. Refreshed every in-combat tick (by the cast/
        //     attack directive that steers it AND by RefreshCombatTarget from the
        //     combat service), RE-POINTED on a target change (same handle), and
        //     released ONLY by the expiry sweep once refreshing STOPS -- i.e. at combat
        //     end. It is NOT tied to the cast gambit's win/lose, so a cast->melee
        //     transition RE-POINTS it, never releases it. Same round-robin caveat as
        //     cast-SELECT above -- "every in-combat tick" means this follower's own
        //     Scheduler::Tick lap, so it too now uses FacetExpiry().
        // Each claim carries its own refresh timestamp. Guarded by g_mx (worker + main).
        // package-offer (ch.9) is a PER-EXCURSION, caller-driven lifecycle wired into
        // Packages.cpp's loot-travel routing: created on the first winning dispatch,
        // refreshed by a repeat call (same form == cheap no-op via EnsureClaimLocked),
        // released the instant the caller says so (arrival/loot-done/abandoned). ITS
        // refresh is genuinely NOT round-robin-bound -- Packages::Pump() refreshes
        // every live package-offer claim UNCONDITIONALLY, for every active slot, on
        // EVERY Scheduler::Tick call (~133ms flat, independent of party size and of
        // whose turn the round-robin cursor is on) -- so the flat kExpiry backstop
        // really is a safe ~3.7x margin here (verified 2026-09-05, not assumed).
        // combat-action-deny (ch.7) shares the SAME claim machinery and was designed
        // to piggyback on that identical Packages.cpp per-excursion cadence, but is
        // currently UNWIRED (no caller anywhere in the tree -- "built but not wired",
        // Docs/STATUS.md) -- actionHandle is dead weight today, so its flat kExpiry is
        // unexercised rather than proven; revisit sizing WHEN it is actually wired,
        // based on whatever actually drives it then.
        struct Owned {
            APMF_API::Handle spellHandle  = APMF_API::kInvalidHandle;  RE::FormID spell  = 0;
            std::chrono::steady_clock::time_point spellRefreshed{};
            APMF_API::Handle targetHandle = APMF_API::kInvalidHandle;  RE::FormID target = 0;
            std::chrono::steady_clock::time_point targetRefreshed{};
            APMF_API::Handle packageHandle = APMF_API::kInvalidHandle;  RE::FormID package = 0;
            std::chrono::steady_clock::time_point packageRefreshed{};
            APMF_API::Handle actionHandle  = APMF_API::kInvalidHandle;  std::uint32_t actionMask = 0;
            std::chrono::steady_clock::time_point actionRefreshed{};
            // weapon-order equipment (ch.15) -- PER-ORDER: refreshed every tick the
            // force-hold survives (Actuation::ReconcileForcedWeapon), released the
            // instant it releases (Actuation::ReleaseForcedWeapon). PROVEN
            // round-robin-bound, not tight-cadence (deck 2026-09-05, Cicero):
            // ReconcileForcedWeapon is called from the SAME per-follower
            // Scheduler::Tick service as cast-select/combat-target above, so "every
            // tick the force-hold survives" is one round-robin lap, same gap as
            // those two. Uses FacetExpiry() below, not the flat kExpiry.
            APMF_API::Handle equipHandle   = APMF_API::kInvalidHandle;  RE::FormID equip  = 0;
            std::chrono::steady_clock::time_point equipRefreshed{};
            // heal-cast +ACT (ch.8, feat/cast-act) -- PER-CAST, declarative. Held
            // by ComposedCast for the life of a claimed heal; refreshed every tick
            // the gambit still wants it (create-or-repoint on a spell/target/hand
            // change), released the instant it stops (ComposedCast::End), and
            // auto-expired by FacetExpiry() (below) if a caller forgets -- same
            // PER-CAST refresh-or-expire shape as spellHandle above, just a
            // distinct slot (heal vs. offense's arbitrate-only claim never overlap
            // on one follower, but each gets its own state to avoid any cross-talk).
            APMF_API::Handle healHandle    = APMF_API::kInvalidHandle;  RE::FormID healSpell  = 0;
            RE::FormID       healTarget    = 0;                          std::int32_t healHand = 0;
            std::chrono::steady_clock::time_point healRefreshed{};
        };
        std::mutex                             g_mx;
        std::unordered_map<RE::FormID, Owned>  g_owned;

        // MFO outbids APMF's mid-range test hotkeys (basis 100) so a real gambit wins
        // the hand/target over a tester's Numpad keys. Arbitrary among clients; > 100.
        constexpr float kOwnBasis = 200.0f;

        // A claim not refreshed within this window is released. Genuinely flat-safe
        // ONLY for facets proven to refresh on a true ~133ms beat regardless of party
        // size -- verified (2026-09-05) to be package-offer alone (Packages::Pump()
        // refreshes it unconditionally, every Scheduler::Tick, for every active slot --
        // NOT gated by the round-robin cursor). combat-action-deny is unwired (no
        // current refresher at all) and inherits this pending real evidence once it is
        // wired. Every OTHER facet (cast-select, combat-target, weapon-order equipment,
        // heal-cast) is refreshed from INSIDE the per-follower Scheduler::Tick
        // ROUND-ROBIN service -- ONE follower serviced per ~133ms pump -- and uses
        // FacetExpiry() below instead.
        constexpr auto kExpiry = std::chrono::milliseconds(500);

        // ROUND-ROBIN-AWARE FACET EXPIRY (deck 2026-09-05, found via the heal-cast
        // claim: claim/release every ~530ms, caster stuck at rest forever; then
        // RE-PROVEN on the weapon-order equipment claim, Cicero deck capture: CLAIMED
        // gate-only -> APMF-side RELEASED 919ms later, a full round-robin lap early,
        // while MFO's own force-hold was still standing -- see ReconcileForcedWeapon's
        // call site for the trace). An earlier version of this comment asserted the
        // flat 500ms kExpiry was "a fine backstop" for cast-select/combat-target/
        // equipment because "a live combat controller re-Wants every combat-thread
        // beat" -- THAT WAS WRONG, disproven by the Cicero capture: all three are
        // refreshed from the SAME per-follower Scheduler::Tick ROUND-ROBIN lap the
        // heal claim uses (ClaimCasting/ClaimCombatTarget <- Actuation::CastOn <-
        // Actuation::Fire <- Scheduler.cpp's round-robin service; ClaimEquipment <-
        // Actuation::ReconcileForcedWeapon <- the SAME service) -- ONE follower
        // serviced per ~133ms (Scheduler.cpp), so a given follower's own gambit only
        // re-fires (and re-Claims/Repoints/refreshes) every ~0.133s * partySize. For
        // anything but a 1-2-follower party that gap already exceeds the flat 500ms,
        // so the sweep in Tick() below released a live, still-wanted claim every
        // round-robin lap. Size it the SAME way TargetCastReconcile/SelfCastReconcile
        // already size their own round-robin-aware release windows: out-wait the
        // worst-case suppression + round-robin gap, floored at the old kExpiry so a
        // small party never regresses to a SLOWER release than before. Shared by
        // spellHandle/targetHandle/equipHandle/healHandle -- one formula, no
        // per-facet copy-paste (none of the four need different sizing: they all
        // share the identical round-robin service as their refresh source).
        std::chrono::milliseconds FacetExpiry() {
            const float suppress  = std::max(0.0f, Config::g_suppressWindow.load());
            const float partySize = static_cast<float>(Followers::g_active.size() + 1);   // + player
            const float sec = std::max(0.5f, suppress * 1.12f + 0.133f * partySize + 0.5f);
            return std::chrono::milliseconds(static_cast<std::uint64_t>(sec * 1000.0f));
        }

        // Ensure ONE channel claim tracks `want` (0 == release it). Caller holds g_mx.
        // On a CHANGE of an existing claim, RE-POINTS in place via Repoint (v3, same
        // handle -- no release/re-engage churn); falls back to Release+Request only
        // against an older v2 APMF. APMF Release/RequestEx/Repoint are thread-safe.
        void EnsureClaimLocked(const APMF_API::APMF_API_v2* api, RE::FormID follower,
                               APMF_API::Intent intent, APMF_API::Handle& handle,
                               RE::FormID& cur, RE::FormID want) {
            if (want == 0) {                                   // no claim wanted
                if (handle != APMF_API::kInvalidHandle) { api->Release(handle); handle = APMF_API::kInvalidHandle; }
                cur = 0;
                return;
            }
            if (handle != APMF_API::kInvalidHandle && cur == want) return;   // unchanged
            APMF_API::APMF_Param p{};
            p.form = want;
            if (handle == APMF_API::kInvalidHandle) {                        // create
                handle = api->RequestEx(follower, intent, kOwnBasis, &p);
                cur    = (handle != APMF_API::kInvalidHandle) ? want : 0;
            } else if (api->abiVersion >= 3) {                               // re-point in place
                reinterpret_cast<const APMF_API::APMF_API_v3*>(api)->Repoint(handle, &p);
                cur = want;
            } else {                                                        // v2 fallback: release+request
                api->Release(handle);
                handle = api->RequestEx(follower, intent, kOwnBasis, &p);
                cur    = (handle != APMF_API::kInvalidHandle) ? want : 0;
            }
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

        // Release one handle (thread-safe; no-ops a stale handle). Caller holds g_mx.
        // RE::FormID IS std::uint32_t (a plain alias, not a distinct type), so this
        // ONE overload also serves actionMask -- a second overload on that "different"
        // parameter type would be a duplicate-definition error, not a real overload.
        void ReleaseHandleLocked(APMF_API::Handle& handle, RE::FormID& cur) {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (api && handle != APMF_API::kInvalidHandle) api->Release(handle);
            handle = APMF_API::kInvalidHandle;
            cur = 0;
        }

        // Heal-cast (+ACT) create-or-repoint: like EnsureClaimLocked, but the
        // claim's identity is the (spell, target, hand) TRIPLE -- all three ride
        // the SAME APMF_Param -- so a change in ANY of them is a re-point, not
        // just the spell. Caller holds g_mx. `wantSpell == 0` releases.
        void EnsureHealClaimLocked(const APMF_API::APMF_API_v2* api, RE::FormID follower,
                                   APMF_API::Handle& handle, RE::FormID& curSpell,
                                   RE::FormID& curTarget, std::int32_t& curHand,
                                   RE::FormID wantSpell, RE::FormID wantTarget, std::int32_t wantHand) {
            if (wantSpell == 0) {
                if (handle != APMF_API::kInvalidHandle) { api->Release(handle); handle = APMF_API::kInvalidHandle; }
                curSpell = 0; curTarget = 0; curHand = 0;
                return;
            }
            if (handle != APMF_API::kInvalidHandle && curSpell == wantSpell &&
                curTarget == wantTarget && curHand == wantHand)
                return;   // unchanged
            APMF_API::APMF_Param p{};
            p.form   = wantSpell;
            p.ival   = wantHand;
            p.target = wantTarget;
            if (handle == APMF_API::kInvalidHandle) {                        // create
                handle = api->RequestEx(follower, APMF_API::kIntent_SelectSpell, kOwnBasis, &p);
            } else if (api->abiVersion >= 3) {                               // re-point in place
                reinterpret_cast<const APMF_API::APMF_API_v3*>(api)->Repoint(handle, &p);
            } else {                                                        // v2 fallback: release+request
                api->Release(handle);
                handle = api->RequestEx(follower, APMF_API::kIntent_SelectSpell, kOwnBasis, &p);
            }
            if (handle != APMF_API::kInvalidHandle) { curSpell = wantSpell; curTarget = wantTarget; curHand = wantHand; }
            else                                     { curSpell = 0; curTarget = 0; curHand = 0; }
        }

        // Drop the map entry once EVERY claim is gone. Caller holds g_mx.
        void EraseIfEmpty(std::unordered_map<RE::FormID, Owned>::iterator it) {
            const auto& o = it->second;
            if (o.spellHandle == APMF_API::kInvalidHandle && o.targetHandle == APMF_API::kInvalidHandle &&
                o.packageHandle == APMF_API::kInvalidHandle && o.actionHandle == APMF_API::kInvalidHandle &&
                o.equipHandle == APMF_API::kInvalidHandle && o.healHandle == APMF_API::kInvalidHandle)
                g_owned.erase(it);
        }
    }

    void Acquire() {
        g_apmf.store(nullptr, std::memory_order_relaxed);
        void* h = GetModuleHandleA("APMF.dll");
        if (!h) {
            spdlog::info("[apmf] interface absent -- APMF.dll not in the load order; "
                         "owned-cast model OFF (MFO falls back to the legacy AI-first cast hybrid).");
            return;
        }
        auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
            GetProcAddress(h, APMF_API::kGetInterfaceExport));
        if (!fn) {
            spdlog::warn("[apmf] APMF.dll present but '{}' not exported -- owned-cast model OFF.",
                         APMF_API::kGetInterfaceExport);
            return;
        }
        const APMF_API::APMF_API_v1* base = fn(APMF_API::kABIVersion);
        if (!base) {
            spdlog::warn("[apmf] APMF refused ABI v{} (too old) -- owned-cast model OFF.",
                         APMF_API::kABIVersion);
            return;
        }
        if (base->abiVersion < 2) {
            spdlog::warn("[apmf] APMF ABI v{} has no RequestEx (need >= 2) -- owned-cast model OFF.",
                         base->abiVersion);
            return;
        }
        auto* api = reinterpret_cast<const APMF_API::APMF_API_v2*>(base);
        g_apmf.store(api, std::memory_order_relaxed);
        spdlog::info("[apmf] interface acquired (ABI v{}) -- owned-cast model enabled "
                     "(target+spell owned; AI fires it ANIMATED; re-point {}).",
                     api->abiVersion, api->abiVersion >= 3 ? "in place" : "via release+request (v2)");
    }

    bool Available() { return g_apmf.load(std::memory_order_relaxed) != nullptr; }

    namespace {
        // The exact toast wording, in ONE place. Plain, one sentence, no em/en-
        // dash, no semicolon (Docs/VOICE.md).
        constexpr const char* kNoApmfToast =
            "MFO works best with Harbinger (APMF) installed, and it's running in fallback mode without it.";

        constexpr double kWarnFirstAtMinutes = 0.6;    // ~36s after load: seen once, early
        constexpr double kWarnEveryMinutes   = 10.0;   // then roughly every 10 minutes

        // -1.0 = never fired yet this session. Rapport::SessionMinutes() resets on
        // every load, so a load that makes it go backwards (mins < last) is this
        // module's own "new session" signal -- no separate reset hook needed, and
        // nothing here touches serialization.
        std::atomic<double> g_warnLastMinutes{ -1.0 };
    }

    void MaybeWarnAbsence() {
        if (Available()) return;                     // APMF present: never warn, one cheap check
        if (!Config::g_warnNoApmf.load()) return;     // silenced

        const double mins = Rapport::SessionMinutes();
        double last = g_warnLastMinutes.load(std::memory_order_relaxed);
        if (mins < last) last = -1.0;                 // a new load happened; treat as a fresh session

        const bool fire = (last < 0.0) ? (mins >= kWarnFirstAtMinutes)
                                        : (mins - last >= kWarnEveryMinutes);
        if (!fire) return;

        g_warnLastMinutes.store(mins, std::memory_order_relaxed);
        MainThread::Post([]() { RE::DebugNotification(kNoApmfToast); });
    }

    bool IsOwnedCastActive(RE::FormID a_follower) {
        // Fast-out before the lock: APMF absent -> never active (mirrors every
        // other accessor's g_apmf check).
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() && it->second.spellHandle != APMF_API::kInvalidHandle;
    }

    // ── cast-SELECT (per-cast) ──────────────────────────────────────────────────
    void ClaimCasting(RE::FormID a_follower, RE::FormID a_spell) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_spell == 0 || !Config::g_apmfCast.load()) return;
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureClaimLocked(api, a_follower, APMF_API::kIntent_SelectSpell, o.spellHandle, o.spell, a_spell);
        o.spellRefreshed = std::chrono::steady_clock::now();
        EraseIfEmpty(g_owned.find(a_follower));
    }

    void ReleaseCasting(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseHandleLocked(it->second.spellHandle, it->second.spell);   // cast-select only; leave combat-target
        EraseIfEmpty(it);
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

    // ── heal-cast +ACT (per-cast, declarative, ch.8, feat/cast-act) ─────────────
    // ival bit 2 = APMF's +ACT opt-in (APMF_API.h kIntent_SelectSpell: bits 0-1
    // are the hand, bit 2 is kActFlag_Drive). CLEAR (offense's ClaimCasting,
    // above) stays pure gate-only -- the client's OWN AI casts, APMF only
    // narrows/denies. SET -> APMF itself equips + animates + fires the cast
    // (core/CastExecutor.cpp) -- exactly what a heal-cast claim needs, since
    // nothing else will ever make the AI choose to cast it. A NAMED constant,
    // never a bare magic number at the call site.
    static constexpr std::int32_t kApmfCastActDrive = 0x4;

    bool ClaimHealCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                       std::int32_t a_hand) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        // Executor toggle = the (repurposed) bHealAnimPackage key. APMF absent /
        // toggle off / no spell -> OFF, degrade to kInstant.
        if (!api || a_follower == 0 || a_spell == 0 || !Config::g_healAnimPackage.load())
            return false;
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureHealClaimLocked(api, a_follower, o.healHandle, o.healSpell, o.healTarget, o.healHand,
                              a_spell, a_target, a_hand | kApmfCastActDrive);
        o.healRefreshed = std::chrono::steady_clock::now();
        const bool live = o.healHandle != APMF_API::kInvalidHandle;
        EraseIfEmpty(g_owned.find(a_follower));
        return live;
    }

    void ReleaseHealCast(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseHandleLocked(it->second.healHandle, it->second.healSpell);
        it->second.healTarget = 0;
        it->second.healHand   = 0;
        EraseIfEmpty(it);
    }

    bool IsHealCastActive(RE::FormID a_follower) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() && it->second.healHandle != APMF_API::kInvalidHandle;
    }

    void Tick() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api) return;
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(g_mx);
        // Computed once per sweep, not per-facet-per-follower: same inputs
        // (Config::g_suppressWindow, Followers::g_active.size()) for every claim
        // checked below, and Tick() already holds g_mx for the whole loop.
        const auto facetExpiry = FacetExpiry();
        for (auto it = g_owned.begin(); it != g_owned.end();) {
            auto& o = it->second;
            if (o.spellHandle  != APMF_API::kInvalidHandle && now - o.spellRefreshed  >= facetExpiry)
                ReleaseHandleLocked(o.spellHandle,  o.spell);
            if (o.targetHandle != APMF_API::kInvalidHandle && now - o.targetRefreshed >= facetExpiry)
                ReleaseHandleLocked(o.targetHandle, o.target);
            // package-offer: genuinely flat-refreshed every ~133ms regardless of party
            // size (Packages::Pump(), unconditional, every active slot) -- kExpiry stays.
            if (o.packageHandle != APMF_API::kInvalidHandle && now - o.packageRefreshed >= kExpiry)
                ReleaseHandleLocked(o.packageHandle, o.package);
            // combat-action-deny: unwired (no caller anywhere), so kExpiry is inert
            // today -- kept flat pending real evidence once something drives it.
            if (o.actionHandle != APMF_API::kInvalidHandle && now - o.actionRefreshed >= kExpiry)
                ReleaseHandleLocked(o.actionHandle, o.actionMask);
            if (o.equipHandle != APMF_API::kInvalidHandle && now - o.equipRefreshed >= facetExpiry)
                ReleaseHandleLocked(o.equipHandle, o.equip);
            if (o.healHandle != APMF_API::kInvalidHandle && now - o.healRefreshed >= facetExpiry) {
                ReleaseHandleLocked(o.healHandle, o.healSpell);
                o.healTarget = 0; o.healHand = 0;
            }
            if (o.spellHandle == APMF_API::kInvalidHandle && o.targetHandle == APMF_API::kInvalidHandle &&
                o.packageHandle == APMF_API::kInvalidHandle && o.actionHandle == APMF_API::kInvalidHandle &&
                o.equipHandle == APMF_API::kInvalidHandle && o.healHandle == APMF_API::kInvalidHandle)
                it = g_owned.erase(it);
            else
                ++it;
        }
    }

    void ClearTransientState() {
        std::scoped_lock lock(g_mx);
        for (auto& [id, o] : g_owned) {
            ReleaseHandleLocked(o.spellHandle,  o.spell);
            ReleaseHandleLocked(o.targetHandle, o.target);
            ReleaseHandleLocked(o.packageHandle, o.package);
            ReleaseHandleLocked(o.actionHandle,  o.actionMask);
            ReleaseHandleLocked(o.equipHandle,   o.equip);
            ReleaseHandleLocked(o.healHandle,    o.healSpell);
            o.healTarget = 0; o.healHand = 0;
        }
        g_owned.clear();
    }
}
