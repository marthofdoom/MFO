#include "APMFBridge.h"
#include "APMF_API.h"
#include "Config.h"
#include "Followers.h"   // g_active.size() -- round-robin-aware expiry sizing (FacetExpiry)
#include "Loadout.h"     // WeaponHandActive's live-grip read
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
        //   * offense-cast (kIntent_Cast, ch.8b): PER-CAST. Refreshed each winning
        //     cast tick; released CRISPLY the moment no cast rule holds
        //     (ReleaseOffenseCast <- Scheduler !castSeen); the expiry is only a
        //     backstop. PORTED feat/offense-cast-seats (2026-09-05) off the retired
        //     ch.8 kIntent_SelectSpell gate-only claim this field used to back --
        //     see Owned::offenseHandle below and APMFBridge.h's ClaimOffenseCast
        //     doc for the full port rationale. IMPORTANT (2026-09-05, corrects an
        //     earlier false assumption): "each winning cast tick" is a
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
        // Each claim carries its own refresh timestamp. Guarded by g_mx (worker + main
        // + the COMBAT THREAD: Phase 2's IsOwnedCastActive/IsEquipmentClaimActive are
        // called from CasterConsent/CombatStyle thunks). Real mutex, never nested, never
        // held across a Post/form-table walk, so the combat thread can't stall on it.
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
            // heal-cast (ch.8b, kIntent_Cast/RequestCast, ported feat/mfo-cast-port)
            // -- PER-CAST, TTL-bounded. Held by ComposedCast for the life of a
            // claimed heal; refreshed every tick the gambit still wants it
            // (create-or-refresh on a spell/target/hand/concentration/stopPct
            // change -- RequestCast has no in-place re-point, so a CHANGE releases
            // and re-requests, a new bounded claim), released the instant it stops
            // (ComposedCast::End), and auto-expired by FacetExpiry() (below) AND
            // (on APMF's side) by the claim's own TTL if a caller forgets -- same
            // PER-CAST refresh-or-expire shape as offenseHandle below, just a
            // distinct slot (heal and offense claims never overlap on one
            // follower -- CasterConsent::SpellKind makes them mutually exclusive
            // per tick -- but each gets its own state to avoid any cross-talk).
            APMF_API::Handle healHandle    = APMF_API::kInvalidHandle;  RE::FormID healSpell  = 0;
            RE::FormID       healTarget    = 0;                          std::int32_t healHand = 0;
            bool              healConc     = false;                      std::uint32_t healStopPct = 0;
            std::chrono::steady_clock::time_point healRefreshed{};
            // offense-cast (ch.8b, kIntent_Cast/RequestCast, PORTED feat/offense-
            // cast-seats, 2026-09-05, off the retired ch.8 kIntent_SelectSpell
            // gate-only claim this slot used to back -- see APMFBridge.h's
            // ClaimOffenseCast doc) -- PER-CAST, TTL-bounded, same shape/lifecycle
            // as healHandle above, just a DISTINCT slot: heal and offense casts
            // are mutually exclusive per tick by CasterConsent::SpellKind, never
            // concurrent on one follower, but each keeps its own state so neither
            // claim's release/refresh ever cross-talks with the other's.
            APMF_API::Handle offenseHandle = APMF_API::kInvalidHandle;  RE::FormID offenseSpell = 0;
            RE::FormID       offenseTarget = 0;                          std::int32_t offenseHand = 0;
            bool              offenseConc  = false;                      std::uint32_t offenseStopPct = 0;
            std::chrono::steady_clock::time_point offenseRefreshed{};
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
    }   // end anon namespace -- FacetExpiry (below) is now EXTERNAL LINKAGE, declared
        // in APMFBridge.h, so feat/cast-gambit-concentration's cross-TU cast-lock
        // staleness window (Actuation.cpp, Task 2) can reuse the SAME round-robin-
        // aware sizing instead of inventing its own budget (#9). Purely a visibility
        // move -- no formula change; every existing in-TU call site below still
        // resolves it via the header declaration (included at the top of this file).

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
    // heal claim uses (ClaimOffenseCast/ClaimCombatTarget <- Actuation::CastOn <-
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
    // offenseHandle/targetHandle/equipHandle/healHandle -- one formula, no
    // per-facet copy-paste (none of the four need different sizing: they all
    // share the identical round-robin service as their refresh source). NOW ALSO
    // reused by Actuation.cpp's cast-gambit lock (Task 2) as the staleness window
    // for its own un-claimed (direct-force concentration) fallback case.
    std::chrono::milliseconds FacetExpiry() {
        const float suppress  = std::max(0.0f, Config::g_suppressWindow.load());
        const float partySize = static_cast<float>(Followers::g_active.size() + 1);   // + player
        const float sec = std::max(0.5f, suppress * 1.12f + 0.133f * partySize + 0.5f);
        return std::chrono::milliseconds(static_cast<std::uint64_t>(sec * 1000.0f));
    }

    namespace {

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

        // kIntent_Cast create-or-refresh (kIntent_Cast/RequestCast, ch.8b) -- SHARED
        // by both ClaimHealCast and ClaimOffenseCast (renamed from
        // EnsureHealClaimLocked, feat/offense-cast-seats, 2026-09-05: the function
        // was already fully generic over its handle/curX out-params, only the name
        // was heal-specific). Unlike EnsureClaimLocked's retired kIntent_SelectSpell
        // Repoint, RequestCast's rich payload has NO in-place re-point -- a CHANGE
        // in any of (spell, target, hand, concentration, stopPct) releases the old
        // claim and requests a fresh one (a new bounded window; never two live
        // claims on the same follower's slot at once). Caller holds g_mx AND must
        // have already verified api->abiVersion >= 5 (RequestCast is a v5 slot --
        // see ClaimHealCast/ClaimOffenseCast). `wantSpell == 0` releases.
        void EnsureCastClaimLocked(const APMF_API::APMF_API_v5* api, RE::FormID follower,
                                   APMF_API::Handle& handle, RE::FormID& curSpell,
                                   RE::FormID& curTarget, std::int32_t& curHand,
                                   bool& curConc, std::uint32_t& curStopPct,
                                   RE::FormID wantSpell, RE::FormID wantTarget, std::int32_t wantHand,
                                   bool wantConc, std::uint32_t wantStopPct) {
            if (wantSpell == 0) {
                if (handle != APMF_API::kInvalidHandle) { api->Release(handle); handle = APMF_API::kInvalidHandle; }
                curSpell = 0; curTarget = 0; curHand = 0; curConc = false; curStopPct = 0;
                return;
            }
            if (handle != APMF_API::kInvalidHandle && curSpell == wantSpell &&
                curTarget == wantTarget && curHand == wantHand &&
                curConc == wantConc && curStopPct == wantStopPct)
                return;   // unchanged -- cheap no-op, no release/re-request churn
            if (handle != APMF_API::kInvalidHandle) { api->Release(handle); handle = APMF_API::kInvalidHandle; }
            APMF_API::APMF_CastRequest req{};
            req.spell  = wantSpell;
            req.proxy  = 0;   // APMF mints its own delivery-flip proxy for kSelf-delivery (core/CastProxy.h)
            req.target = wantTarget;
            // kCastFlag_DualCast and kCastFlag_LeftHand are mutually exclusive (APMF_API.h:
            // "do NOT set kCastFlag_LeftHand alongside it") -- kApmfHandDualCast (Loadout::
            // HandPick::DualCast, via HandFor above) claims BOTH hands and never also sets
            // the single-hand hint.
            const bool wantDual = (wantHand == kApmfHandDualCast);
            req.flags  = (wantDual                    ? APMF_API::kCastFlag_DualCast :
                          wantHand == kApmfHandLeft    ? APMF_API::kCastFlag_LeftHand : 0u) |
                         (wantConc ? APMF_API::kCastFlag_Concentration : 0u) |
                         APMF_API::MakeStopPct(wantStopPct);
            req.ttlMs  = kHealCastTtlMs;
            handle = api->RequestCast(follower, kOwnBasis, &req);
            // [cfc] dual-cast ask vs. observed outcome (marth 2026-09-06): the flag is a
            // HINT (APMF_API.h) -- APMF may still only arm one hand, and never reports which.
            // This distinguishes "asked for dual, claim granted" (engine may still degrade to
            // one hand silently downstream) from "asked for dual, claim REFUSED outright" from
            // "never asked" (no log at all) -- so the field can tell a refused second hand from
            // a policy that never requested one. Fires only on a claim CREATE/CHANGE (the
            // unchanged-claim fast path above already skips repeat ticks), so this is
            // inherently rate-limited, not spammy.
            if (wantDual) {
                if (handle != APMF_API::kInvalidHandle)
                    spdlog::info("[cfc] {:08X} asked for dual-cast (spell {:08X}) -- claim "
                                 "granted; engine may still arm only one hand (hint only)",
                                 follower, wantSpell);
                else
                    spdlog::warn("[cfc] {:08X} asked for dual-cast (spell {:08X}) -- claim "
                                 "REFUSED outright, not just downgraded to one hand",
                                 follower, wantSpell);
            }
            if (handle != APMF_API::kInvalidHandle) {
                curSpell = wantSpell; curTarget = wantTarget; curHand = wantHand;
                curConc  = wantConc;  curStopPct = wantStopPct;
            } else {
                curSpell = 0; curTarget = 0; curHand = 0; curConc = false; curStopPct = 0;
            }
        }

        // Drop the map entry once EVERY claim is gone. Caller holds g_mx.
        void EraseIfEmpty(std::unordered_map<RE::FormID, Owned>::iterator it) {
            const auto& o = it->second;
            if (o.targetHandle == APMF_API::kInvalidHandle &&
                o.packageHandle == APMF_API::kInvalidHandle && o.actionHandle == APMF_API::kInvalidHandle &&
                o.equipHandle == APMF_API::kInvalidHandle && o.healHandle == APMF_API::kInvalidHandle &&
                o.offenseHandle == APMF_API::kInvalidHandle)
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
        return it != g_owned.end() && it->second.offenseHandle != APMF_API::kInvalidHandle;
    }

    // ── offense-cast (per-cast, TTL-bounded, ch.8b, kIntent_Cast/RequestCast) ───
    // PORTED feat/offense-cast-seats (2026-09-05) off the retired ch.8
    // kIntent_SelectSpell gate-only claim (ClaimCasting/ReleaseCasting, removed)
    // this call site used to make -- see APMFBridge.h's ClaimOffenseCast doc for
    // the full port rationale; this is just the claim plumbing, mirroring
    // ClaimHealCast's shape exactly via the shared EnsureCastClaimLocked helper.
    bool ClaimOffenseCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                         std::int32_t a_hand, bool a_concentration, std::uint32_t a_stopPct) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_spell == 0 || !Config::g_apmfCast.load())
            return false;
        // RequestCast is a v5 slot; an APMF.dll built before it (ABI < 5) has no
        // such function pointer to call at all -- degrade cleanly to the legacy
        // AI-first-grace + force-on-miss hybrid rather than reading past the end
        // of an older, shorter interface struct. Logged ONCE (own flag, separate
        // from ClaimHealCast's -- a session may exercise only one of the two
        // paths and both deserve their own visibility).
        if (api->abiVersion < 5) {
            static std::atomic<bool> s_warnedOffense{ false };
            if (!s_warnedOffense.exchange(true))
                spdlog::warn("[apmf] ABI v{} has no RequestCast (need >= 5) -- offense-cast claim "
                             "OFF; the legacy AI-first-grace hybrid runs instead (degrade).", api->abiVersion);
            return false;
        }
        auto* v5 = reinterpret_cast<const APMF_API::APMF_API_v5*>(api);
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureCastClaimLocked(v5, a_follower, o.offenseHandle, o.offenseSpell, o.offenseTarget, o.offenseHand,
                              o.offenseConc, o.offenseStopPct, a_spell, a_target, a_hand,
                              a_concentration, a_stopPct);
        o.offenseRefreshed = std::chrono::steady_clock::now();
        const bool live = o.offenseHandle != APMF_API::kInvalidHandle;
        EraseIfEmpty(g_owned.find(a_follower));
        return live;
    }

    void ReleaseOffenseCast(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseHandleLocked(it->second.offenseHandle, it->second.offenseSpell);   // offense-cast only; leave combat-target
        it->second.offenseTarget  = 0;
        it->second.offenseHand    = 0;
        it->second.offenseConc    = false;
        it->second.offenseStopPct = 0;
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

    // ── heal-cast (per-cast, TTL-bounded, ch.8b, kIntent_Cast/RequestCast) ──────
    // Ported feat/mfo-cast-port (2026-09-05): APMF's feat/ai-cast-seats-impl
    // retired the kIntent_SelectSpell +ACT drive this used to ride (`ival`'s
    // hand-mode bits and kActFlag_Drive opt-in are now accepted-and-ignored on
    // that channel -- APMF_API.h) and replaced it with five engine vfunc seats
    // answered while a kIntent_Cast claim stands, so the NPC's OWN AI drives the
    // animated cast natively. See APMFBridge.h's doc comment above for the full
    // shape; this is just the claim plumbing.
    bool ClaimHealCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                       std::int32_t a_hand, bool a_concentration, std::uint32_t a_stopPct) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        // Executor toggle = the (repurposed) bHealAnimPackage key. APMF absent /
        // toggle off / no spell -> OFF, degrade to kInstant.
        if (!api || a_follower == 0 || a_spell == 0 || !Config::g_healAnimPackage.load())
            return false;
        // RequestCast is a v5 slot; an APMF.dll built before it (ABI < 5) has no
        // such function pointer to call at all -- degrade cleanly to kInstant
        // rather than reading past the end of an older, shorter interface struct
        // (the SAME guard shape as SetSpellAllowList's `>= 4` check above).
        // Logged ONCE (not every tick) so a stale APMF.dll is legible without
        // spamming the log every heal attempt.
        if (api->abiVersion < 5) {
            static std::atomic<bool> s_warned{ false };
            if (!s_warned.exchange(true))
                spdlog::warn("[apmf] ABI v{} has no RequestCast (need >= 5) -- heal-cast claim "
                             "OFF; heals apply via kInstant (degrade).", api->abiVersion);
            return false;
        }
        auto* v5 = reinterpret_cast<const APMF_API::APMF_API_v5*>(api);
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureCastClaimLocked(v5, a_follower, o.healHandle, o.healSpell, o.healTarget, o.healHand,
                              o.healConc, o.healStopPct, a_spell, a_target, a_hand,
                              a_concentration, a_stopPct);
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
        it->second.healTarget  = 0;
        it->second.healHand    = 0;
        it->second.healConc    = false;
        it->second.healStopPct = 0;
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
            if (o.offenseHandle != APMF_API::kInvalidHandle && now - o.offenseRefreshed >= facetExpiry) {
                ReleaseHandleLocked(o.offenseHandle, o.offenseSpell);
                o.offenseTarget = 0; o.offenseHand = 0; o.offenseConc = false; o.offenseStopPct = 0;
            }
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
                o.healTarget = 0; o.healHand = 0; o.healConc = false; o.healStopPct = 0;
            }
            if (o.targetHandle == APMF_API::kInvalidHandle &&
                o.packageHandle == APMF_API::kInvalidHandle && o.actionHandle == APMF_API::kInvalidHandle &&
                o.equipHandle == APMF_API::kInvalidHandle && o.healHandle == APMF_API::kInvalidHandle &&
                o.offenseHandle == APMF_API::kInvalidHandle)
                it = g_owned.erase(it);
            else
                ++it;
        }
    }

    void ClearTransientState() {
        std::scoped_lock lock(g_mx);
        for (auto& [id, o] : g_owned) {
            ReleaseHandleLocked(o.targetHandle, o.target);
            ReleaseHandleLocked(o.packageHandle, o.package);
            ReleaseHandleLocked(o.actionHandle,  o.actionMask);
            ReleaseHandleLocked(o.equipHandle,   o.equip);
            ReleaseHandleLocked(o.healHandle,    o.healSpell);
            o.healTarget = 0; o.healHand = 0; o.healConc = false; o.healStopPct = 0;
            ReleaseHandleLocked(o.offenseHandle, o.offenseSpell);
            o.offenseTarget = 0; o.offenseHand = 0; o.offenseConc = false; o.offenseStopPct = 0;
        }
        g_owned.clear();
    }
}
