// apmf/Bridge.cpp -- the APMF client CORE: the interface pointer and the claim
// map it guards (g_apmf, g_mx, g_owned), the shared claim helpers
// (EnsureClaimLocked / EnsureCastClaimLocked / ReleaseClaimLocked, the idle-hand
// floor), FacetExpiry, Acquire / Available / MaybeWarnAbsence, the per-tick
// expiry sweep (Tick) and ClearTransientState. Was the head of
// native/APMFBridge.cpp; the wave-1 subsystem-folder split (2026-09-24) moved
// each claim family to its own file in apmf/.
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

// Two Win32 symbols, declared by hand. <windows.h> is BANNED outside Board.cpp --
// it #defines GetObject and hijacks BGSDefaultObjectManager::GetObject<T>
// (ENGINE_NOTES §9; the same reason Targeting.cpp / Logistics.cpp hand-declare
// GetModuleHandleA). Pointer args/return are pointer-sized on Win64, so void* is
// ABI-correct for HMODULE/FARPROC; the real header is never in this TU to conflict.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char* a_name);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void* a_module, const char* a_procName);

namespace MFO::APMFBridge {

        // The APMF interface (v2+, so RequestEx is present; Repoint needs v3 -- gated
        // per call on api->abiVersion). Written ONCE at kDataLoaded (main) before any
        // worker tick runs; read on the worker. Atomic (relaxed) enforces the
        // publish/consume contract cheaply.
        std::atomic<const APMF_API::APMF_API_v2*> g_apmf{ nullptr };

        std::mutex                             g_mx;
        std::unordered_map<RE::FormID, Owned>  g_owned;
        // ch.17 claim-refusal error throttle (once per refusal streak, per
        // follower). Outside Owned on purpose: a refused claim leaves no handle,
        // so its Owned entry is erased and could not carry the latch. Under g_mx.
        std::unordered_set<RE::FormID>         g_equipAuthRefused;
        // ch.8 gate-only claim: the SAME once-per-streak throttles, for the same
        // reason (a refused/overflowing follower leaves no handle to latch on).
        // Under g_mx. `g_selectOverflow` = the allow-list did not fit, so no gate is
        // held; `g_selectRefused` = APMF refused the claim.
        std::unordered_set<RE::FormID>         g_selectOverflow;
        std::unordered_set<RE::FormID>         g_selectRefused;

    namespace {
        // ── never-published warn throttle (SEV-3, pre-merge review 2026-09-07) ──
        // Failing closed leaves the claim EMPTY, so the next winning lap mints again
        // and the lap after that lands on the same branch: unthrottled, the warn fires
        // every SECOND lap (~3.75 Hz solo) for as long as the refusal persists, and
        // buries the caller's headline line under its own noise (principle 8). Same
        // 5 s per (follower, spell) window as Actuation's LogApmfRefusal, so the two
        // stay in step. Guarded by g_mx like every other map here -- the only writer
        // is EnsureCastClaimLocked, and every caller of that holds the lock.
        //
        // TWO ENTRIES PER FOLLOWER, NOT ONE (F10, 2026-09-08). The key used to be
        // the follower alone, with the last-warned SPELL as the "is this the same
        // situation?" test. With the idle-hand floor a follower now holds a
        // DRIVING claim (spell X) and a DENY-ONLY floor (spell 0) at the same
        // time, both walking this same path on the same laps -- so a single entry
        // would flip X <-> 0 on every call and NEITHER would ever be throttled,
        // turning a 5 s window into a line per lap. Index [0] = driving claims,
        // [1] = the floor: each gets its own window and the two cannot cancel
        // each other out.
        constexpr auto kNeverLiveWarnEvery = std::chrono::milliseconds(5000);
        struct NeverLiveWarn {
            RE::FormID                            spell = 0;
            std::chrono::steady_clock::time_point when{};
        };
        std::unordered_map<RE::FormID, std::array<NeverLiveWarn, 2>> g_lastNeverLiveWarn;

        // ── cast-claim HEARTBEAT log throttle (F5-1, 2026-09-08) ────────────────
        // A heartbeat that silently fails is WORSE than no heartbeat, because RC2
        // would then look fixed while the gap was still open (principle 5: do not
        // build an unobservable mechanism). So every renewal is loggable -- but at
        // one line per renewal per claim, a party mid-fight would emit a line every
        // few hundred ms across its live cast claims and bury the headline cast
        // lines under its own bookkeeping (principle 8). Same shape and the same
        // 5 s per (follower, spell) window as g_lastNeverLiveWarn above, so the two
        // cast-claim diagnostics in this file stay in step: a NEW spell always logs
        // its first renewal (the crossing that proves the mechanism fires at all),
        // and a long-standing claim logs at most every 5 s thereafter.
        //
        // THE LINE IS SELF-PROVING, NOT A COUNT. It carries the claim's AGE since
        // its mint, so ONE line whose age exceeds kHealCastTtlMs is by itself proof
        // that the window was RENEWED rather than re-requested -- the throttle can
        // drop lines without costing the field its answer. Nothing here retries or
        // masks: a renewal that does not take shows up on the very next lap as the
        // existing "already auto-expired -- re-requesting" line below (principle 7).
        //
        // TWO ENTRIES PER FOLLOWER for exactly the reason g_lastNeverLiveWarn
        // above has two (F10): a driving claim and the idle-hand floor heartbeat
        // on the same laps, and one shared entry would defeat both windows.
        constexpr auto kRenewLogEvery = std::chrono::milliseconds(5000);
        struct RenewLog {
            RE::FormID                            spell = 0;
            std::chrono::steady_clock::time_point when{};
        };
        std::unordered_map<RE::FormID, std::array<RenewLog, 2>> g_lastRenewLog;

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
    // offense[0]/offense[1]/targetHandle/equipHandle/heal -- one formula, no
    // per-facet copy-paste (none of these need different sizing: they all
    // share the identical round-robin service as their refresh source). NOW ALSO
    // reused by Actuation.cpp's cast-gambit lock (Task 2) as the staleness window
    // for its own un-claimed (direct-force concentration) fallback case.
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

        // Release ONE CastClaim's handle (if live) and reset it to default.
        // Caller holds g_mx. Safe to call on an already-empty claim (no-op).
        void ReleaseClaimLocked(CastClaim& c) {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);
            c = CastClaim{};
        }

    namespace {
        // ── HOW OFTEN A LIVE kIntent_Cast CLAIM IS HEARTBEATEN (F5-1, 2026-09-08) ──
        // Derived, never a convenient number (#9): the two inputs are the claim's
        // OWN granted TTL (kHealCastTtlMs, the value this file passes as
        // req.ttlMs for every cast claim it makes) and MFO's OWN worst-case refresh
        // cadence, which is already sized ONCE in this file as FacetExpiry() -- the
        // round-robin-aware window a claim may go un-refreshed before the Tick()
        // sweep gives up on it. Reusing FacetExpiry() rather than inventing a
        // second budget is the point: a heartbeat sized off a number the sweep
        // does not share is exactly the "two budgets that can disagree" failure
        // this codebase keeps paying for.
        //
        //   interval = min( TTL/2 , TTL - FacetExpiry() ), floored at 0
        //
        //   * TTL/2 (3000ms at kHealCastTtlMs=6000) is the ordinary heartbeat
        //     halving: renewing at half-life means a claim survives even if ONE
        //     scheduled renewal is missed entirely, which is the common case here
        //     because renewals ride the round-robin lap and a lap can be skipped
        //     (the follower's rule loses a tick, the party grows, the game stalls).
        //   * TTL - FacetExpiry() is the hard ceiling: FacetExpiry() is this file's
        //     own stated upper bound on the gap between two consecutive services of
        //     one follower, so any interval above it could let the window lapse
        //     between two renewals. At the defaults (fSuppressWindow 1.5, solo
        //     party) FacetExpiry() is ~2446ms, so this ceiling is ~3554ms and the
        //     TTL/2 term wins; a big party or a wide suppression window pulls the
        //     ceiling down and it takes over -- e.g. fSuppressWindow 3.0 with 5
        //     followers gives ~4658ms and an interval of ~1342ms.
        //   * The floor of 0 (FacetExpiry() >= TTL) degrades honestly to "renew on
        //     every service": if MFO tolerates a longer un-refreshed gap than APMF
        //     grants, there is no safe interval left and the cheapest correct thing
        //     is to renew whenever we are here. No masking either way -- a claim
        //     that lapses regardless still takes the existing aged-out path.
        //
        // NOT AN EXPIRY. Nothing is released on this value; it only decides when to
        // send a Repoint. Every release decision stays where it already was.
        std::chrono::milliseconds CastHeartbeatInterval() {
            const auto ttl   = std::chrono::milliseconds(kHealCastTtlMs);
            const auto half  = ttl / 2;
            const auto lap   = FacetExpiry();
            const auto slack = (lap < ttl) ? (ttl - lap) : std::chrono::milliseconds(0);
            return (half < slack) ? half : slack;
        }
    }

        // kIntent_Cast create-or-refresh (kIntent_Cast/RequestCast, ch.8b) -- SHARED
        // by both ClaimHealCast and ClaimOffenseCast (renamed from
        // EnsureHealClaimLocked, feat/offense-cast-seats, 2026-09-05: the function
        // was already fully generic over its handle/curX out-params, only the name
        // was heal-specific; feat/per-hand-cast-slots, 2026-09-06: folded the six
        // parallel out-params into one CastClaim& now that offense needs TWO of
        // them). Unlike EnsureClaimLocked's retired kIntent_SelectSpell Repoint,
        // RequestCast's rich payload has NO in-place re-point -- a CHANGE in any of
        // (spell, target, hand, concentration, stopPct) releases the old claim and
        // requests a fresh one (a new bounded window; never two live claims on the
        // same slot at once). Caller holds g_mx AND must have already verified
        // api->abiVersion >= 5 (RequestCast is a v5 slot -- see ClaimHealCast/
        // ClaimOffenseCast). `wantSpell == 0` releases.
        //
        // `wantDenyOnly` (F10, 2026-09-08) makes this ALSO the mint/renew path for
        // the IDLE-HAND FLOOR -- a claim that drives nothing and only closes its
        // hand (APMF_API::kCastFlag_DenyHandOnly). Deliberately the SAME function
        // rather than a parallel one: the floor needs the identical liveness
        // check, TTL heartbeat, never-published fail-closed split and reqParam
        // mirror this already implements, and a copy of ~100 lines of that is how
        // two paths drift. Three consequences the caller must know:
        //   * `wantSpell == 0` no longer means "release" on its own -- a floor
        //     carries spell 0 for its whole life BY DEFINITION (APMF forces it to
        //     0 anyway). The release sentinel is now "no spell AND not a floor".
        //     A floor is released by ReleaseClaimLocked at its own call sites.
        //   * `denyOnly` is part of the claim's IDENTITY, so a floor can never be
        //     silently reinterpreted as a driving claim (or vice versa) by the
        //     unchanged fast path below.
        //   * wantTarget/wantConc/wantStopPct are meaningless for a floor and are
        //     passed as 0/false/0 by its caller; APMF zeroes the target anyway.
        void EnsureCastClaimLocked(const APMF_API::APMF_API_v5* api, RE::FormID follower,
                                   CastClaim& c, RE::FormID wantSpell, RE::FormID wantTarget,
                                   std::int32_t wantHand, bool wantConc, std::uint32_t wantStopPct,
                                   bool wantDenyOnly) {
            // Which of the two per-follower log-throttle slots this claim uses --
            // see g_lastNeverLiveWarn's comment for why a floor and a driving
            // claim must never share one.
            const std::size_t logSlot = wantDenyOnly ? 1u : 0u;
            if (wantSpell == 0 && !wantDenyOnly) {
                ReleaseClaimLocked(c);
                return;
            }
            if (c.handle != APMF_API::kInvalidHandle && c.spell == wantSpell &&
                c.target == wantTarget && c.hand == wantHand &&
                c.conc == wantConc && c.stopPct == wantStopPct &&
                c.denyOnly == wantDenyOnly) {
                // ABI v6 (cast-claim observability, 2026-09-06): do NOT trust a
                // stored handle blindly on the unchanged fast path -- APMF
                // auto-expires a claim at its own TTL with no notice to the
                // client (APMF_API.h's IsClaimLive doc), so MFO could keep
                // believing a long-dead handle is still in force (the exact
                // field failure diagnosed 2026-09-06: the gambit re-fired for
                // 23s while this early-return kept swallowing every re-fire and
                // RequestCast never reached APMF again). ABI < 6 has no way to
                // ask this and keeps today's behaviour byte-identical.
                //
                // LATENT, AND THE CORRECTNESS DEPENDENCY IS DOCUMENTED NOWHERE
                // ELSE (Fable diff review, 2026-09-06). IsClaimLive walks APMF's
                // PUBLISHED snapshot only (core/ControlMap.cpp) -- the same shape
                // as the GetCastProxy bug F4 fixes just below -- so a handle
                // RequestCast minted in THIS frame, before APMF's next per-frame
                // Drain publishes it, reads as NOT live. Called in the same frame
                // as a fresh RequestCast, this branch therefore logs "already
                // auto-expired" and re-requests a claim that is in fact alive.
                // THE INVARIANT IS NARROWER THAN "ONE heal Try() PER TICK", and
                // the broad version stated here was wrong (round-3 review,
                // 2026-09-07): Logistics really does run several heal Try()s per
                // follower per tick (a Held outcome continues the scan, and the
                // `pass < 2 && !acted` wrapper re-runs it). What cannot happen is
                // a second ClaimHealCast on the SAME tick as one that MINTED a
                // handle: a Claimed outcome ends every scan, and a Refused one
                // leaves no incumbent behind for a later Try() to hold. So every
                // call that reaches this branch is looking at a handle minted on
                // an EARLIER tick, which APMF has long since published. THAT is
                // the undocumented dependency this fast path's correctness rests
                // on -- break it and this thrashes for that tick.
                // ABI < 6 cannot answer IsClaimLive at all, so it can neither observe
                // liveness nor detect a drain-time loss -- it keeps today's behaviour
                // byte-identical and is treated as "live", exactly as before.
                const bool liveNow =
                    api->abiVersion < 6 ||
                    reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->IsClaimLive(c.handle);
                if (liveNow) c.everLive = true;   // F3-1: latch, never cleared for this handle
                if (liveNow) {
                    // F4 (RC4, 2026-09-06): the proxy is minted lazily during
                    // APMF's own per-frame Drain, so the GetCastProxy read in
                    // the mint block below (:769-771) can still see 0 even
                    // though the SAME handle now has a real proxy published.
                    // Re-read it lazily here, on the unchanged/still-live fast
                    // path this claim takes on every later Try() while nothing
                    // changes, so the watch eventually learns it instead of
                    // caching that 0 forever.
                    if (c.proxy == 0 && api->abiVersion >= 6)
                        c.proxy = reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->GetCastProxy(c.handle);
                    // ── CAST-CLAIM HEARTBEAT (F5-1, 2026-09-08; RC2 of
                    //    Docs/DIAG-2026-09-06-deny-heal-failures.md) ─────────────
                    // THE HALF THIS FILE WAS MISSING. A kIntent_Cast claim is
                    // bounded by design (APMF design.md 5a -- never a standing
                    // hold), and until now MFO let it hard-expire and then
                    // re-requested it on the next lap. Measured cost of that
                    // cycle in the field: the actor went UNCLAIMED for 0.3-1.2 s
                    // every 6 s, and a foreign spell was seen equipping and
                    // charging 110 ms into one of those gaps -- i.e. the gap is
                    // not theoretical, the AI walks straight into it. APMF's own
                    // half of the fix is merged (ControlMap::ApplyRepoint's TTL
                    // RENEWAL: a Repoint on a LIVE cast claim moves its deadline
                    // to now + the claim's OWN granted, already-clamped ttlMs),
                    // but it was unreachable, because MFO never Repointed a cast
                    // claim at all -- this early return was the only path a
                    // still-wanted claim ever took, and it returned without
                    // renewing anything. So the 6 s TTL becomes what principle 9
                    // asks for: a renewable FLOOR that a crashed or uninterested
                    // client still loses on the original schedule, instead of an
                    // expiry that kills LIVE state.
                    //
                    // WHY IT IS STILL BOUNDED. Nothing here holds a claim open by
                    // itself. This code only runs from ClaimHealCast /
                    // ClaimOffenseCast -- i.e. from a rule that WON this lap and
                    // is asking for the identical (spell,target,hand,conc,stopPct)
                    // it already holds. The moment the rule stops winning, the
                    // heartbeat stops with it and the claim dies at APMF's TTL,
                    // exactly as before; MFO's own explicit releases and the
                    // Tick()/FacetExpiry() sweep are untouched. Deliberately NOT
                    // added to RefreshHealCastClaim: that one is ComposedCast's
                    // incumbent HOLD, which by design keeps `refreshed` moving for
                    // a claim whose rule may have gone quiet, and renewing APMF's
                    // TTL from there would take away the very liveness bound that
                    // hold documents (see RefreshHealCastClaim's bound 1).
                    //
                    // ABI >= 6 ONLY, and that is a correctness gate, not caution.
                    // On ABI < 6 `liveNow` above is an ASSUMPTION (there is no
                    // IsClaimLive to ask), so a Repoint from here could be aimed at
                    // a handle APMF expired long ago -- and it would then log a
                    // renewal that never happened, which is a mask (#7). With a
                    // positive IsClaimLive answer in hand the renewal is real:
                    // ApplyRepoint refuses to resurrect an already-lapsed claim, so
                    // "live now" is precisely the precondition it renews under.
                    // ABI < 6 keeps today's behaviour byte-identical, the same
                    // discipline the liveness check itself and RefreshHealCastClaim
                    // already follow. Inert in practice (kABIVersion is 6 and the
                    // DLL pair ships together).
                    //
                    // THE PARAM IS THE STORED ONE, NEVER A REBUILD -- see
                    // CastClaim::reqParam above for why (ApplyRepoint REFUSES a
                    // form change on a cast claim, loudly, and a rebuilt flags word
                    // is free to drift from what was actually requested).
                    if (api->abiVersion >= 6) {
                        const auto now = std::chrono::steady_clock::now();
                        // ONE read, reused by the test AND the log below.
                        // CastHeartbeatInterval() bottoms out in FacetExpiry(),
                        // whose inputs (fSuppressWindow, party size) are live --
                        // calling it twice could log a threshold that is not the
                        // one the decision was made on, which is a diagnostic
                        // that lies (principle 5).
                        const auto every = CastHeartbeatInterval();
                        if (now - c.renewed >= every) {
                            api->Repoint(c.handle, &c.reqParam);
                            const auto sinceRenew = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        now - c.renewed).count();
                            const auto age        = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        now - c.created).count();
                            c.renewed = now;
                            if (auto& r = g_lastRenewLog[follower][logSlot];
                                r.spell != wantSpell || now - r.when >= kRenewLogEvery) {
                                r.spell = wantSpell;
                                r.when  = now;
                                spdlog::info("[apmf] {:08X} kIntent_Cast {} (spell {:08X}) HEARTBEAT -- "
                                             "Repointed in place, renewing APMF's {}ms TTL (claim age {}ms, "
                                             "{}ms since the last renewal, next in ~{}ms). A LIVE claim whose "
                                             "age exceeds the TTL is the proof the window was RENEWED, not "
                                             "re-requested; a renewal that did not take shows up on the next "
                                             "lap as the 'already auto-expired' line instead.",
                                             follower,
                                             wantDenyOnly ? "IDLE-HAND FLOOR (deny-only)" : "claim",
                                             wantSpell, kHealCastTtlMs, age, sinceRenew,
                                             every.count());
                            }
                        }
                    }
                    return;   // unchanged AND still live -- cheap no-op, no release/re-request churn
                }
                // ── NOT LIVE. WHICH KIND? (F3-1, deploy-gate review 2026-09-07) ──
                // Until this split, both kinds took the re-request path below, and
                // that made the whole fail-closed model nearly unreachable on the
                // APMF that actually ships: ControlMap::EnqueueCast returns
                // kInvalidHandle SYNCHRONOUSLY in exactly one case (no channel serves
                // kIntent_Cast at all) -- every real ARBITRATION decision happens
                // later, in Drain(). So a claim that LOST arbitration still came back
                // with a handle, reported Claimed, took the opaque "AI deciding" NoOp
                // that walls off every rule below, read not-live on the next tick, and
                // was silently re-requested at INFO forever. A refusal that loops
                // quietly while the follower does nothing is precisely the masked
                // failure this work exists to remove (principle 7).
                if (c.everLive) {
                    // AGED OUT: this claim WAS in force and hit APMF's own TTL. Normal
                    // and expected -- re-request, exactly as before this split.
                    spdlog::info("[apmf] {:08X} kIntent_Cast claim (spell {:08X}) already auto-expired "
                                 "at APMF's own TTL -- re-requesting instead of trusting a dead handle",
                                 follower, wantSpell);
                    c = CastClaim{};   // treat as empty; fall through to the fresh RequestCast below
                } else {
                    // NEVER PUBLISHED: minted on an EARLIER tick (this branch is only
                    // ever reached by a later call -- see the dependency note above)
                    // and not once seen live since.
                    //
                    // WHAT THIS CATCHES, AND WHAT IT DOES NOT (corrected by the
                    // pre-merge review 2026-09-07 -- the mechanism is right, but the
                    // premise it was built on was too broad, and an over-claim in a
                    // comment is how the NEXT diagnosis goes wrong). APMF `push_back`s
                    // an enqueued claim REGARDLESS of who ends up owning the facet, and
                    // `IsClaimLive` answers true for ANY unexpired claim carrying that
                    // handle, owner or not. So an ordinary BASIS or TIE loss still
                    // reads live, still latches `everLive`, and is INVISIBLE here --
                    // MFO reports Claimed while APMF drives someone else's spell.
                    //   * CAUGHT: an OUTRIGHT refusal -- APMF accepted the request and
                    //     then published no claim at all (dual-vs-single hand collision
                    //     loser, unloadable actor, FromPackage-without-spell).
                    //   * NOT CAUGHT: losing arbitration on basis, or a tie.
                    // Do NOT read this branch as "MFO detects arbitration losses" --
                    // it does not, and cannot yet.
                    //
                    // FOLLOW-UP `apmf-isclaimowning-v7` (OPEN, tracked -- not a note).
                    // Separating a basis/tie loss from an outright refusal needs a real
                    // owner query, `IsClaimOwning(handle)`: an APMF v7 ABI addition
                    // (append-only `APMF_API.h` slot + a ControlMap owner lookup) with
                    // an MFO consumer change here behind an `abiVersion >= 7` guard,
                    // exactly the shape of the ABI-6 IsClaimLive/GetCastProxy adoption
                    // above. It does not exist today; until it does, this split is the
                    // most MFO can honestly claim.
                    //
                    // Report it as a refusal: drop the handle and leave the claim EMPTY
                    // without re-requesting, so ClaimOffenseCast/ClaimHealCast return
                    // false and the caller FAILS CLOSED and logs loudly, naming the
                    // actor/spell/target/hand under its own 5 s throttle.
                    //
                    // NOT A LATCH: the claim is left empty, so the next winning tick
                    // makes a genuinely fresh ask. If APMF grants it, everything
                    // resumes; if it keeps losing, the caller keeps failing closed and
                    // saying so. The one false positive this can produce is a stall
                    // long enough that APMF's Drain never ran between the mint and this
                    // check (a paused game): that costs ONE loud line and ONE skipped
                    // cast tick, and self-corrects on the next ask. A visible,
                    // self-correcting false alarm is the right trade against an
                    // invisible permanent loop.
                    if (auto& w = g_lastNeverLiveWarn[follower][logSlot];
                        w.spell != wantSpell ||
                        std::chrono::steady_clock::now() - w.when >= kNeverLiveWarnEvery) {
                        w.spell = wantSpell;
                        w.when  = std::chrono::steady_clock::now();
                        spdlog::warn("[apmf] {:08X} kIntent_Cast claim (spell {:08X}) was minted but NEVER "
                                     "published as live -- treating as an OUTRIGHT refusal (hand-collision "
                                     "loss / unloadable actor), not re-requesting (fail closed)",
                                     follower, wantSpell);
                    }
                    ReleaseClaimLocked(c);
                    return;
                }
            }
            if (c.handle != APMF_API::kInvalidHandle) { api->Release(c.handle); c.handle = APMF_API::kInvalidHandle; }
            APMF_API::APMF_CastRequest req{};
            req.spell  = wantSpell;
            req.proxy  = 0;   // APMF mints its own delivery-flip proxy for kSelf-delivery (core/CastProxy.h)
            req.target = wantTarget;
            // kCastFlag_DualCast and kCastFlag_LeftHand are mutually exclusive (APMF_API.h:
            // "do NOT set kCastFlag_LeftHand alongside it") -- kApmfHandDualCast (Loadout::
            // HandPick::DualCast, via HandFor above) claims BOTH hands and never also sets
            // the single-hand hint. Anything else (kApmfHandRight, or bare 0) is the RIGHT
            // hint -- APMF's own default when kCastFlag_LeftHand is unset (APMF_API.h).
            const bool wantDual = (wantHand == kApmfHandDualCast);
            // F10: kCastFlag_DenyHandOnly is ORTHOGONAL to the hand hint and rides
            // ALONGSIDE it -- the floor stands on exactly the hand
            // kCastFlag_LeftHand selects (APMF_API.h). It is never combined with
            // kCastFlag_DualCast: a floor is by construction the ONE hand the
            // driving claim leaves free, and a dual claim leaves none (see
            // ReconcileHandFloorLocked, which never asks for a dual floor).
            req.flags  = (wantDual                    ? APMF_API::kCastFlag_DualCast :
                          wantHand == kApmfHandLeft    ? APMF_API::kCastFlag_LeftHand : 0u) |
                         (wantConc     ? APMF_API::kCastFlag_Concentration : 0u) |
                         (wantDenyOnly ? APMF_API::kCastFlag_DenyHandOnly  : 0u) |
                         APMF_API::MakeStopPct(wantStopPct);
            req.ttlMs  = kHealCastTtlMs;
            c.handle = api->RequestCast(follower, kOwnBasis, &req);
            // [cfc] dual-cast ask vs. observed outcome (marth 2026-09-06): the flag is a
            // HINT (APMF_API.h) -- APMF may still only arm one hand, and never reports which.
            // This distinguishes "asked for dual, claim granted" (engine may still degrade to
            // one hand silently downstream) from "asked for dual, claim REFUSED outright" from
            // "never asked" (no log at all) -- so the field can tell a refused second hand from
            // a policy that never requested one. Fires only on a claim CREATE/CHANGE (the
            // unchanged-claim fast path above already skips repeat ticks), so this is
            // inherently rate-limited, not spammy.
            if (wantDual) {
                if (c.handle != APMF_API::kInvalidHandle)
                    spdlog::info("[cfc] {:08X} asked for dual-cast (spell {:08X}) -- claim "
                                 "granted; engine may still arm only one hand (hint only)",
                                 follower, wantSpell);
                else
                    spdlog::warn("[cfc] {:08X} asked for dual-cast (spell {:08X}) -- claim "
                                 "REFUSED outright, not just downgraded to one hand",
                                 follower, wantSpell);
            }
            if (c.handle != APMF_API::kInvalidHandle) {
                c.spell = wantSpell; c.target = wantTarget; c.hand = wantHand;
                c.conc  = wantConc;  c.stopPct = wantStopPct;
                c.denyOnly = wantDenyOnly;
                // Fable SEV-1 (2026-09-06): the claim's AGE clock, stamped HERE
                // and only here -- this is the single point where a NEW handle
                // comes into existence. The unchanged-and-still-live fast path
                // above deliberately does NOT touch it, which is exactly what
                // makes it a bound (see the `created` field comment above and
                // RefreshHealCastClaim below).
                c.created = std::chrono::steady_clock::now();
                // F5-1: APMF's TTL window starts HERE, at the RequestCast above, so
                // the heartbeat clock starts here too. Unlike `created` this one DOES
                // move -- every renewal below advances it -- which is exactly why the
                // two are separate fields (see CastClaim::renewed).
                c.renewed = c.created;
                // F5-1: the param APMF now stores for this claim, captured verbatim
                // for the heartbeat Repoint. This MUST mirror APMF's
                // ControlMap::EnqueueCast, which builds a zero-initialised
                // APMF_Param and writes exactly two fields from the APMF_CastRequest
                // it was handed: `param.form = req->spell` and
                // `param.ival = req->flags` (kept in parity with castFlags). Anything
                // else stays zero on both sides. Re-sending this on a heartbeat is
                // therefore a no-change param update plus a TTL renewal, which is
                // precisely the "same-form heartbeat" APMF_API.h invites and the one
                // shape that trips none of ApplyRepoint's refusals.
                //
                // `form` MIRRORS ApplyRequest's EFFECTIVE form, not `req.spell` --
                // see CastClaim::reqParam above for the full working. A
                // kCastFlag_DenyHandOnly claim has its driven form forced to 0 on
                // the APMF side, and ApplyRepoint pins it to 0 there too, so
                // re-sending `wantSpell` on such a claim would earn a refusal warn
                // every single heartbeat. Mirrored here instead of assumed. (The
                // OTHER rewrite, kCastFlag_FromPackage, is NOT representable from
                // this side and MFO sets neither flag today -- documented above.)
                c.reqParam       = APMF_API::APMF_Param{};
                c.reqParam.form  = (req.flags & APMF_API::kCastFlag_DenyHandOnly) ? 0u : wantSpell;
                c.reqParam.ival  = static_cast<std::int32_t>(req.flags);
                // ABI v6: fetch the delivery-flip proxy APMF minted for this claim
                // (0 on ABI < 6, or a claim that minted no proxy) -- recorded
                // alongside the claimed spell so a later match on the OBSERVED
                // cast (Diagnostics.cpp's SpellSink, via ComposedCast's watch) can
                // recognise a cast of the proxy as our own claim actually firing,
                // not "their own spell, not ours" (the false-alarm diagnosed
                // 2026-09-06 -- MFO only ever matched the original spell).
                c.proxy = (api->abiVersion >= 6)
                    ? reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->GetCastProxy(c.handle)
                    : 0;
            } else {
                c = CastClaim{};
            }
        }

    namespace {
        // ── THE IDLE-HAND FLOOR, DERIVED (F10, 2026-09-08) ──────────────────────
        // Caller holds g_mx. Brings `o.floor` into agreement with the driving
        // claims standing beside it, and does nothing else. Called from ONE place
        // (Tick(), below) so the floor can never disagree with itself across six
        // call sites -- see that call for the cadence argument.
        //
        // THE RULE, and the whole of it:
        //   exactly ONE hand driven  -> floor the OTHER hand
        //   both hands driven        -> no floor (a DualCast claim, or a claim on
        //                               each hand: nothing is left open)
        //   NEITHER hand driven      -> NO FLOOR ON EITHER HAND
        //
        // WHY "NEITHER", NOT "BOTH" (the explicit design question, answered here
        // rather than picked). Four reasons, in the order that decided it:
        //   1. DECLARE -> ENFORCE, and MFO has declared nothing. Principle 4 says
        //      enforce only what was declared and never fabricate un-given input.
        //      A cast gambit holding one hand is MFO saying "I am driving this
        //      follower's casting right now"; the floor is the rest of that same
        //      sentence. With no cast claim at all MFO is saying nothing about
        //      casting, and a floor would be MFO enforcing an intent no rule
        //      expressed.
        //   2. It would mute followers nobody asked to mute. A party member with
        //      no cast gambit authored at all -- a pure melee or ranged follower,
        //      or any follower whose cast rules' conditions are simply false --
        //      would be silenced for the whole game by a rule they never opted
        //      into. marth's ruling is about the hand MFO LEAVES OVER while it
        //      drives ("the followers OTHER hand"), not about followers MFO is
        //      not driving.
        //   3. A floor with nothing driving is a STANDING HOLD, which kIntent_Cast
        //      is explicitly never allowed to be (APMF design.md 5a: always
        //      bounded). As a companion it inherits its bound from the claim it
        //      accompanies -- when the gambit stops winning, both stop being
        //      renewed and both die at APMF's TTL. On its own it would need a
        //      heartbeat with no natural end.
        //   4. It would be unreadable in the field. A follower who casts nothing,
        //      with no gambit running and no MFO line to explain it, is
        //      indistinguishable from MFO being broken -- and that is the hardest
        //      failure shape to diagnose (principles 5 and 7).
        //
        // THE BASIS IS kOwnBasis, THE SAME ONE EVERY OTHER MFO CLAIM USES -- and
        // that is load-bearing, not laziness. APMF's shared comparator
        // (core/ControlMap.h BetterClaim, used at EVERY winner selection on the
        // cast channel) ranks by basis, and adds ONE rule: AT AN EQUAL BASIS A
        // DENY-ONLY CLAIM LOSES TO A DRIVING ONE. So MFO can put the floor down
        // first and still have its own next gambit take that hand the instant the
        // gambit's claim publishes -- no release/re-request gap and no self-deny
        // -- with the floor still standing underneath when the gambit ends
        // (APMF_API.h, kCastFlag_DenyHandOnly, "*** BASIS: THE TIE IS ENFORCED FOR
        // YOU. ***"). A floor at a HIGHER basis would be a strict inversion that
        // silences MFO's own casts, and APMF warns loudly about it. Requesting the
        // floor at kOwnBasis is therefore how supersession is expressed, not an
        // omission -- do NOT give it its own basis constant.
        //
        // IT DOES NOT DENY MFO'S OWN DIRECT FORCE -- re-verified 2026-09-08
        // against the PINNED CommonLibSSE-NG (3.7.0 @ c4ab853d,
        // include/RE/M/MagicCaster.h), not inherited from the earlier claim.
        // `CastSpellImmediate` is vtable slot 01 (:46) and `CheckCast` is slot 0A
        // (:55) -- two different virtuals, and APMF's gates hook CheckCast (0x0A)
        // and CheckShouldEquip (0x0F), which is the AI's own deliberation, never
        // the immediate-cast entry. So the APMF-ABSENT / legacy / concentration
        // direct-force paths (Actuation_Direct.cpp's
        // GetMagicCaster(kInstant)->CastSpellImmediate) pass a floored hand
        // untouched. What the floor closes is the AI's deliberation on that hand,
        // which is exactly and only what it claims to do. Weapons are untouched
        // too -- the 0x0F seat is installed on the spell/staff selector vtables
        // only (APMF core/EquipGate.cpp's own "weapon/fist item classes have no
        // concrete header class to hook"), so a spellsword's off-hand steel is
        // not disarmed by a floor.
        void ReconcileHandFloorLocked(const APMF_API::APMF_API_v5* api, RE::FormID follower, Owned& o) {
            // A DualCast claim is mirrored into BOTH offense slots (see
            // ClaimOffenseCast), so it reads as both hands driven with no special
            // case. Heals are LEFT always (ClaimHealCast's own hard rule).
            //
            // "DRIVEN" MEANS "MFO HOLDS A HANDLE HERE", NOT "APMF HAS PUBLISHED IT"
            // -- deliberately, and with a known cost (review finding, 2026-09-08).
            // Every other consumer of these slots defines driven the same way
            // (IsOwnedCastActiveOnHand, IsHealCastActive, EraseIfEmpty, Tick's own
            // sweep), and a SECOND definition of the word here is exactly the kind
            // of drift that produces two subsystems disagreeing about which hand is
            // busy. What it costs, stated rather than hidden: a driving claim in
            // never-published limbo still reads as driven, so a claim APMF ends up
            // REFUSING buys one pump's worth of floor mint-then-release; and a
            // FLOOR whose own first liveness check misses is dropped by
            // EnsureCastClaimLocked's fail-closed branch and re-minted on the next
            // pump, once every ~133 ms until it publishes. Both are bounded by the
            // refusal itself resolving, both are the SAME shape driving claims have
            // had since the fail-closed split shipped, and the log is throttled
            // (5 s per follower per slot) while the RequestCast traffic is not.
            const bool leftDriven  = o.offense[0].handle != APMF_API::kInvalidHandle ||
                                     o.heal.handle       != APMF_API::kInvalidHandle;
            const bool rightDriven = o.offense[1].handle != APMF_API::kInvalidHandle;

            std::int32_t wantHand = 0;
            if (leftDriven && !rightDriven)      wantHand = kApmfHandRight;
            else if (rightDriven && !leftDriven) wantHand = kApmfHandLeft;

            // THE UNOBSERVED GATE (fix/mfo-combat-restoration-direct, 2026-09-21).
            // A driving claim earns the floor only while it can be believed to be
            // DRIVING: younger than kIdleFloorUnobservedMs, or observed firing
            // (spell or its delivery-flip proxy, ComposedCast's per-hand watch)
            // within its own lifetime. A claim past that age that the engine has
            // never fired is a hand held shut for nothing -- and the floor it
            // earned held the OTHER hand shut too (deck 2026-09-21, Jesper: left on
            // a Fast Healing claim the engine kept re-deliberating away from, right
            // floored, no dagger, frozen). The claim itself is NOT touched here:
            // its own bounds (TTL, FacetExpiry sweep, the hold caps) end it. The
            // watch is armed by every claim site (CastOn's owned branch, the two
            // concentration-offense claims, Try) with the ORIGINAL spell, which is
            // what `c.spell` holds; a claim whose watch was never armed reads as
            // silent past the gate and opens the other hand -- the pre-F10 state,
            // never a freeze. `created` is the mint-only age clock (an aged-out
            // re-request re-mints and restarts the window, documented at the field).
            //
            // RE-SIZED AND ARMED ON CHARGE (fix/mfo-spell-authority-0922, from the
            // 2026-09-22 deck log this gate's own note asked for). Two changes, and
            // the second is the one that matters:
            //   * the age is 8000 ms, not the old 4000 ms alias -- see
            //     kIdleFloorUnobservedMs in APMFBridge.h for the measurement (an
            //     offense claim reaches an observed SpellFire in 5.8 s with an equip
            //     cycle in front, so 4000 ms fired early on EVERY offense claim);
            //   * a claim the ENGINE IS ACTUALLY CHARGING is never silent, whatever
            //     its age. On 09-22 the floor lifted 60 ms AFTER the engine began
            //     charging the claimed Firebolt and the AI charged its own spell in
            //     the freed hand 0.78 s later -- the gate opened a hand out from
            //     under a LIVE cast, which no timer can prevent by being longer.
            //     `Actuation::CastInFlightOnHand` is THE one definition of in-flight
            //     in this codebase (the claim's own hand's MagicCaster in a live cast
            //     state with the claim's spell-or-proxy selected); a SECOND notion of
            //     "is this cast happening" is explicitly refused. Same AddTask job
            //     worker this Tick() runs on, which is the thread that predicate is
            //     already called from (Actuation_Hands' held-re-aim cap,
            //     ComposedCast::Try), and it is a plain member read off the actor's
            //     already-built caster array -- no virtual call, no allocation.
            //     The actor lookup is the established worker-road spelling
            //     (RE::TESForm::LookupByID<RE::Actor>); a null actor (unloaded,
            //     between cells) simply leaves the age test as the only word, which
            //     is the pre-existing behaviour.
            const auto now = std::chrono::steady_clock::now();
            // Resolved ONCE per pass, and only when a floor is actually in question
            // (`wantHand != 0` is the only branch `silentPastGate` is called from) --
            // this runs for every follower with any claim on every ~133 ms pump.
            RE::Actor* actor = wantHand != 0 ? RE::TESForm::LookupByID<RE::Actor>(follower) : nullptr;
            auto silentPastGate = [&](const CastClaim& c) {
                if (c.handle == APMF_API::kInvalidHandle) return true;   // not driving at all
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.created);
                if (age < std::chrono::milliseconds(kIdleFloorUnobservedMs)) return false;
                if (ComposedCast::ObservedFiring(follower, c.hand, c.spell,
                                                 static_cast<std::uint32_t>(age.count())))
                    return false;
                // ARMED ON CHARGE: the engine has this claim's spell (or its
                // delivery-flip proxy) selected on the claim's OWN hand and is in a
                // live cast state -- it is not a hand held shut for nothing.
                const std::size_t hand = (c.hand == kApmfHandLeft) ? Actuation::kHandLeft
                                                                   : Actuation::kHandRight;
                return !Actuation::CastInFlightOnHand(actor, hand, c.spell, c.proxy);
            };
            if (wantHand != 0) {
                const bool driverSilent =
                    wantHand == kApmfHandRight
                        ? (silentPastGate(o.offense[0]) && silentPastGate(o.heal))   // left drives
                        : silentPastGate(o.offense[1]);                               // right drives
                if (driverSilent) {
                    if (o.floor.handle != APMF_API::kInvalidHandle) {
                        const auto hadHand = o.floor.hand;
                        ReleaseClaimLocked(o.floor);
                        spdlog::info("[apmf] {:08X} IDLE-HAND FLOOR released -- driving claim has no "
                                     "observed cast ({} hand floor dropped; the {} hand's claim stood "
                                     ">= {} ms unfired)",
                                     follower, hadHand == kApmfHandLeft ? "left" : "right",
                                     wantHand == kApmfHandRight ? "left" : "right",
                                     kIdleFloorUnobservedMs);
                    }
                    return;   // no floor while the driver is silent; it returns once observed
                }
            }

            if (wantHand == 0) {
                if (o.floor.handle != APMF_API::kInvalidHandle) {
                    const auto hadHand = o.floor.hand;
                    ReleaseClaimLocked(o.floor);
                    spdlog::info("[apmf] {:08X} IDLE-HAND FLOOR released ({} hand) -- MFO no longer holds "
                                 "exactly one driving cast claim, so there is no 'other hand' to close "
                                 "(both hands driven, or none).",
                                 follower, hadHand == kApmfHandLeft ? "left" : "right");
                }
                return;
            }

            // Log on the TRANSITION only (a new floor, or a floor that moved to
            // the other hand). The steady state is silent by construction: the
            // renewals below take EnsureCastClaimLocked's unchanged fast path,
            // whose own heartbeat line is already throttled to one per 5 s.
            const bool announce = o.floor.handle == APMF_API::kInvalidHandle ||
                                  o.floor.hand   != wantHand;
            EnsureCastClaimLocked(api, follower, o.floor, /*wantSpell=*/0, /*wantTarget=*/0,
                                  wantHand, /*wantConc=*/false, /*wantStopPct=*/0,
                                  /*wantDenyOnly=*/true);
            o.floor.refreshed = std::chrono::steady_clock::now();
            if (announce && o.floor.handle != APMF_API::kInvalidHandle)
                spdlog::info("[apmf] {:08X} IDLE-HAND FLOOR claimed ({} hand, deny-only) -- MFO drives the "
                             "{} hand, so nothing un-gambited may arm on this one. It drives nothing and "
                             "admits nothing (not even MFO's own spells); MFO's next gambit takes the hand "
                             "by the equal-basis tie rule, with no release/re-request gap.",
                             follower, wantHand == kApmfHandLeft ? "left" : "right",
                             wantHand == kApmfHandLeft ? "right" : "left");
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
        // REQUEST MFO's MINIMUM ABI, NOT the header's. APMF_GetInterface returns null
        // for any request ABOVE its own version and its NEWEST struct for any request
        // at or below it (APMF ClientAPI.cpp). Asking for kABIVersion (13 since the
        // ch.20 mirror) would turn a v10-v12 Harbinger (e.g. the released 0.9.8, ABI
        // v12) into "APMF absent" and drop EVERY facet. 10 is what main requested
        // before the v13 mirror (the ABI that first serves ch.19); every newer facet
        // gates itself on api->abiVersion (ch.20: TargetPinOffered, >= 13).
        constexpr std::uint32_t kRequestAbi = 10;
        const APMF_API::APMF_API_v1* base = fn(kRequestAbi);
        if (!base) {
            spdlog::warn("[apmf] APMF refused ABI v{} (too old) -- owned-cast model OFF.", kRequestAbi);
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

    void Tick() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api) return;
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(g_mx);
        // ch.20 target pins: notice a pin Harbinger ended (IsClaimLive false) so the
        // next gambit pass re-picks instead of believing a dead pin (apmf/Excursion.cpp).
        SweepTargetPinsLocked(api, now);
        // ch.21 combat entries: notice an entry Harbinger ended, so the engage-on-sight
        // gambit records its target as given up (no re-request loop) and may pick again.
        SweepCombatEntriesLocked(api);
        // Computed once per sweep, not per-facet-per-follower: same inputs
        // (Config::g_suppressWindow, Followers::g_active.size()) for every claim
        // checked below, and Tick() already holds g_mx for the whole loop.
        const auto facetExpiry = FacetExpiry();
        for (auto it = g_owned.begin(); it != g_owned.end();) {
            auto& o = it->second;
            // offense[0]/[1]: PER-HAND now (feat/per-hand-cast-slots) -- each
            // slot expires independently. A mirrored DualCast claim shares ONE
            // handle across both slots and keeps identical `refreshed` stamps
            // (ClaimOffenseCast stamps both together), so it goes stale on the
            // SAME sweep pass for both -- release the shared handle exactly
            // once (capture the dual-ness BEFORE releasing slot i, since
            // releasing zeroes it).
            // Each swept claim also drops ITS hand's [cfc] watch (fix/mfo-combat-
            // restoration-direct, 2026-09-21): this road released the claim and
            // left the watch armed, so the next claim of the same spell inherited a
            // dead `since` (deck: `[cfc] claim live 32383 ms` for a claim swept 19 s
            // earlier). Per-hand (ComposedCast::ClearWatchHand), so a live claim on
            // the other hand keeps its own record; a mirrored DualCast clears both.
            for (std::size_t i = 0; i < 2; ++i) {
                auto& c = o.offense[i];
                if (c.handle != APMF_API::kInvalidHandle && now - c.refreshed >= facetExpiry) {
                    const bool sharedDual = o.offense[1 - i].handle == c.handle;
                    ReleaseClaimLocked(c);
                    // Hand 0's slot is shared with a heal claim (Try arms it too);
                    // whichever armed last owns the record, so leave it while a
                    // heal still stands there -- mirrored below for the heal sweep.
                    if (i == 1 || o.heal.handle == APMF_API::kInvalidHandle)
                        ComposedCast::ClearWatchHand(it->first, i == 0 ? kApmfHandLeft : kApmfHandRight);
                    if (sharedDual) {
                        o.offense[1 - i] = CastClaim{};
                        if (i == 0 || o.heal.handle == APMF_API::kInvalidHandle)
                            ComposedCast::ClearWatchHand(it->first, i == 0 ? kApmfHandRight : kApmfHandLeft);
                    }
                }
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
            // ── ch.8 CAST-SELECT GATE-ONLY CLAIM (fix/mfo-spell-authority-0922) ──
            // Republished from CasterConsent::NoteGambits on every PARTY-COMBAT
            // service lap -- the SAME per-follower round-robin lap cast-select,
            // combat-target and weapon-order equipment are refreshed on -- so
            // FacetExpiry() (round-robin-aware), never the flat kExpiry. This
            // sweep is the mute backstop, not a nicety: the gate DENIES, and a
            // follower who stops being serviced (cell change, a pump stall, a
            // release path that missed him) must LOSE it rather than keep it. No
            // claim means ALLOW on APMF's side, so expiring is the safe direction.
            // The kill switch rides here too, on the pump's cadence, so
            // bApmfSpellAllowList=0 (or bApmfCast=0) mid-session lifts every
            // standing gate at once -- including a follower in combat, whom the
            // release paths only reach at the end of his fight.
            if (o.selectHandle != APMF_API::kInvalidHandle &&
                (now - o.selectRefreshed >= facetExpiry ||
                 !Config::g_apmfSpellAllowList.load() || !Config::g_apmfCast.load())) {
                RE::FormID dummy = 0;
                ReleaseHandleLocked(o.selectHandle, dummy);
                o.selectList.clear();
                spdlog::info("[cast-select] {:08X}: allow-list gate released by the pump ({}) -- the AI's "
                             "own spell selection is its own again",
                             it->first,
                             (!Config::g_apmfSpellAllowList.load() || !Config::g_apmfCast.load())
                                 ? "kill switch off" : "not republished within FacetExpiry");
            }
            if (o.heal.handle != APMF_API::kInvalidHandle && now - o.heal.refreshed >= facetExpiry) {
                ReleaseClaimLocked(o.heal);
                // Heals are LEFT always; an offense claim still live on the left
                // shares that watch slot and must keep it (Try arms hand 0 for the
                // heal, and the two cannot both be watched there at once -- the
                // slot's spell is whichever armed last, so clear only if it is ours).
                if (o.offense[0].handle == APMF_API::kInvalidHandle)
                    ComposedCast::ClearWatchHand(it->first, kApmfHandLeft);
            }
            // ── THE IDLE-HAND FLOOR (F10, 2026-09-08) ───────────────────────
            // Reconciled HERE, and only here, AFTER every sweep above -- so it
            // is always derived from the driving claims as they stand at the end
            // of this pass, never from a set that is about to change two lines
            // later.
            //
            // WHY ONE CALL SITE AND NOT SIX. The floor is a pure FUNCTION of
            // which hands MFO drives, and that set changes in six places
            // (ClaimOffenseCast, ClaimHealCast, the two Release*Cast calls, the
            // sweeps above, and a claim MFO re-requests after an APMF expiry).
            // Reconciling at each would be six chances for the derivation to
            // drift; reconciling on the pump makes it ONE, and Tick() is the one
            // service that runs for EVERY follower on EVERY pump (~133ms,
            // Diagnostics.cpp) regardless of whose turn the per-follower
            // round-robin is on. The cost is that the floor engages/disengages
            // up to one pump late -- ~8 frames, against a measured equip+charge
            // cycle of 2.3-4.5 s for any spell the AI could sneak in, so the
            // window is not one an un-gambited cast can fit through.
            //
            // IT ALSO OWNS THE FLOOR'S REFRESH. This pass stamps
            // `floor.refreshed` every pump and takes EnsureCastClaimLocked's
            // unchanged fast path, which is where the TTL heartbeat lives -- so
            // the floor is renewed on the pump's cadence, not the round-robin's,
            // and needs no FacetExpiry sweep of its own (nothing else refreshes
            // it, so no sweep could ever fire on it while this runs). The bound
            // is unchanged and honest: if the pump stops, the floor stops being
            // renewed and dies at APMF's own TTL like every other cast claim.
            //
            // Gated on bApmfCast, the same global switch every driving cast
            // claim in this file already answers to, so turning the cast facet
            // off cannot leave a floor standing behind it.
            if (api->abiVersion >= 5 && Config::g_apmfCast.load())
                ReconcileHandFloorLocked(reinterpret_cast<const APMF_API::APMF_API_v5*>(api),
                                         it->first, o);
            else if (o.floor.handle != APMF_API::kInvalidHandle)
                ReleaseClaimLocked(o.floor);
            // ch.17 EQUIP AUTHORITY: STANDING -- never expiry-swept (no cadence
            // sizes it; it ends by explicit Release). The ONE thing this pump does
            // for it is the kill switch: bApmfEquipAuthority flipped OFF mid-
            // session must not leave a declared set enforced on a follower MFO has
            // stopped declaring for (APMF would keep refusing the AI's equips
            // against a frozen set). Released here, on the pump's cadence, for
            // every follower at once -- including one in combat, whom the OOC
            // logistics service (the other release road) never reaches.
            if (o.equipAuthHandle != APMF_API::kInvalidHandle && !Config::g_apmfEquipAuthority.load()) {
                RE::FormID dummy = 0;
                ReleaseHandleLocked(o.equipAuthHandle, dummy);
                spdlog::info("[equip-auth] {:08X}: release (bApmfEquipAuthority off)", it->first);
            }
            if (o.targetHandle == APMF_API::kInvalidHandle &&
                o.packageHandle == APMF_API::kInvalidHandle && o.actionHandle == APMF_API::kInvalidHandle &&
                o.equipHandle == APMF_API::kInvalidHandle && o.heal.handle == APMF_API::kInvalidHandle &&
                o.offense[0].handle == APMF_API::kInvalidHandle &&
                o.offense[1].handle == APMF_API::kInvalidHandle &&
                o.floor.handle == APMF_API::kInvalidHandle &&
                o.equipAuthHandle == APMF_API::kInvalidHandle)
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
            ReleaseClaimLocked(o.heal);
            // Mirrored DualCast claim: release the shared handle once (see
            // Tick()'s identical dedupe for why -- a stray second Release on
            // the same handle is never safe to assume harmless).
            const bool sharedDual = o.offense[0].handle != APMF_API::kInvalidHandle &&
                                     o.offense[0].handle == o.offense[1].handle;
            ReleaseClaimLocked(o.offense[0]);
            if (sharedDual) o.offense[1] = CastClaim{};
            else            ReleaseClaimLocked(o.offense[1]);
            // F10: the idle-hand floor goes with the claims it accompanies. This
            // is the dismissal/revert/load teardown, and g_owned is cleared right
            // below -- so a floor left standing here would never be reconciled
            // again and would hold the hand shut until APMF's TTL, with MFO no
            // longer holding the handle that could release it.
            ReleaseClaimLocked(o.floor);
            // ch.17: the standing equip-authority claim goes too (kPreLoadGame /
            // revert). APMF wipes its own control map at its kPreLoadGame, so this
            // is the harmless stale-handle Release the file header describes; the
            // declaration's change detector lives in Logistics and is cleared by
            // Logistics::ClearTransientState on the same road, so the first
            // service after the load re-claims AND re-declares.
            { RE::FormID dummy = 0; ReleaseHandleLocked(o.equipAuthHandle, dummy); }
            // ch.8 gate-only claim: same road, same reasoning. A gate left standing
            // across a revert/load would be a DENY with no MFO-side handle able to
            // lift it. The g_ctrl set this gate mirrors is cleared on the same
            // load window by `CasterConsent::ClearTransientState` -> `ClearAll`
            // (`Serialization.cpp:718`, the revert callback) while this runs from
            // plugin.cpp's kPreLoadGame/kNewGame -- two different messages inside
            // one drained-pump window, not one call, so neither can leave the other
            // holding state for a world that no longer exists.
            { RE::FormID dummy = 0; ReleaseHandleLocked(o.selectHandle, dummy); }
            o.selectList.clear();
        }
        g_owned.clear();
        ClearTargetPinsLocked();   // ch.20 pins: APMF drops them at its own kPreLoadGame; stale Release is a no-op
        ClearCombatEntriesLocked();   // ch.21 entries: same (never saved on APMF's side either)
        g_equipAuthRefused.clear();
        g_selectOverflow.clear();
        g_selectRefused.clear();
    }
}
