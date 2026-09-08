#include "APMFBridge.h"
#include "APMF_API.h"
#include "Config.h"
#include "Followers.h"   // g_active.size() -- round-robin-aware expiry sizing (FacetExpiry)
#include "Loadout.h"     // WeaponHandActive's live-grip read
#include "MainThread.h"
#include "Rapport.h"

#include <array>   // F10: the two-slot (driving / idle-hand floor) log throttles
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
        //     see Owned::offense below (feat/per-hand-cast-slots, 2026-09-06:
        //     now a 2-slot array, one per hand) and APMFBridge.h's
        //     ClaimOffenseCast doc for the full port rationale. IMPORTANT
        //     (2026-09-05, corrects an
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
        // A single kIntent_Cast claim's full state -- ONE handle plus the
        // (spell,target,hand,concentration,stopPct) it was created/refreshed
        // with, plus its own refresh timestamp. Factored out (feat/per-hand-
        // cast-slots, 2026-09-06) so `Owned::offense` can hold TWO of these
        // (one per hand) without duplicating six parallel fields per slot; heal
        // stays a single instance (always LEFT, never per-hand).
        struct CastClaim {
            APMF_API::Handle handle = APMF_API::kInvalidHandle;
            RE::FormID       spell  = 0;
            RE::FormID       target = 0;
            std::int32_t     hand   = 0;
            bool             conc   = false;
            std::uint32_t    stopPct = 0;
            // IS THIS THE IDLE-HAND FLOOR? (F10, 2026-09-08.) A claim carrying
            // APMF_API::kCastFlag_DenyHandOnly drives NOTHING: APMF forces its
            // spell, proxy and target to 0 when the claim is built
            // (APMF core/ControlMap.cpp ApplyRequest) and pins `param.form` to 0
            // on every Repoint, so it exists only to CLOSE its hand to the AI.
            // Part of this claim's IDENTITY, not a decoration: a floor and a
            // driving claim are different asks even when every other field
            // happens to match (both carry spell 0 only in the floor's case, but
            // the identity compare in EnsureCastClaimLocked must not be able to
            // mistake one for the other), and it selects the DenyHandOnly bit in
            // the request flags plus the `reqParam.form = 0` mirror the heartbeat
            // depends on.
            bool             denyOnly = false;
            std::chrono::steady_clock::time_point refreshed{};
            // WHEN THIS HANDLE WAS REQUESTED -- the claim's own AGE clock, set
            // ONCE where `handle` is assigned below and never moved again while
            // the same handle stands (Fable SEV-1, 2026-09-06). Distinct from
            // `refreshed` on purpose: `refreshed` is a heartbeat every interested
            // party bumps (ClaimHealCast, RefreshHealCastClaim), so it can never
            // bound anything a still-interested rule keeps asking for. `created`
            // is bumped by NOTHING -- not a refresh, not APMF-side liveness, not
            // a TTL renewal (APMF's Repoint renewal extends the claim on APMF's
            // side; it does not re-request a handle here) -- so it is the one
            // clock that can cap how long a NEVER-OBSERVED claim is allowed to
            // hold ComposedCast's single heal slot. Reset to the epoch by
            // `c = CastClaim{}` on every release/re-request, so a genuinely new
            // claim always gets a full fresh window. See RefreshHealCastClaim.
            std::chrono::steady_clock::time_point created{};
            // WAS THIS HANDLE EVER PUBLISHED AS LIVE? (F3-1, deploy-gate review
            // 2026-09-07.) Set true the first time IsClaimLive(handle) answers true,
            // and NEVER cleared while the same handle stands; reset to false with the
            // rest of the claim by `c = CastClaim{}` on every release/re-request, so
            // it always describes THIS handle and no earlier one.
            //
            // It exists to separate the only two ways a stored handle can read
            // not-live, which look identical at the call site but mean opposite
            // things:
            //   * everLive == true  -> the claim WAS in force and has since AGED OUT
            //     at APMF's own TTL. Normal, expected, and a re-request is right.
            //   * everLive == false -> APMF minted a handle, and it has never once
            //     been published as live. Handles are minted synchronously and
            //     arbitration is decided later in APMF's Drain (ControlMap.cpp), so
            //     this is the signature of an ARBITRATION LOSS: a refusal that
            //     arrives asynchronously instead of as a kInvalidHandle. Re-requesting
            //     it forever is what made the fail-closed path nearly unreachable.
            bool everLive = false;
            // ABI v6 observability (cast-claim observability, 2026-09-06): the
            // delivery-flip proxy FormID APMF minted internally for `handle`
            // (APMF_API::APMF_API_v6::GetCastProxy), fetched once right after a
            // successful RequestCast. 0 on ABI < 6, or when this claim minted no
            // proxy. Read-only bookkeeping -- exposed to callers (GetHealCastProxy/
            // GetOffenseCastProxy below) so ComposedCast's silent-claim watch can
            // recognise a cast of the PROXY, not just the original spell, as our
            // own claim actually firing (see ComposedCast.h/.cpp).
            RE::FormID       proxy  = 0;
            // ── CAST-CLAIM HEARTBEAT STATE (F5-1, 2026-09-08) ──────────────
            // WHEN APMF'S OWN TTL WINDOW LAST STARTED for this handle -- set
            // where `created` is set (the mint), and moved forward by EVERY
            // successful heartbeat Repoint below. This is the ONLY clock that
            // tracks APMF's `expiresMs`, and it is deliberately none of the
            // other three:
            //   * `created` must NOT move (it is the never-observed hold cap's
            //     bound -- see its comment above and RefreshHealCastClaim);
            //   * `refreshed` is bumped by every interested party every lap
            //     (ClaimHealCast/ClaimOffenseCast/RefreshHealCastClaim), so it
            //     measures MFO-side interest, never time-since-renewal;
            //   * APMF's `expiresMs` lives on APMF's side and is not readable.
            // NOT a second expiry (#9 / the sweep must not disagree with it):
            // nothing is ever released on this stamp. It only decides WHEN to
            // send the next renewal; the release decisions stay exactly where
            // they were (FacetExpiry()'s sweep on `refreshed`, the explicit
            // Release* calls, and -- on APMF's side -- the TTL itself).
            std::chrono::steady_clock::time_point renewed{};
            // THE EXACT APMF_Param APMF STORED FOR THIS CLAIM, kept verbatim so
            // a heartbeat Repoint can re-send it byte-for-byte instead of
            // rebuilding one that is merely "the same".
            //
            // WHAT IT MUST MIRROR IS `ApplyRequest`'s **EFFECTIVE** FORM, NOT
            // `req->spell` (review finding, 2026-09-08 -- the sentence that stood
            // here named only EnqueueCast and so recorded a dependency it did not
            // state). TWO functions shape what APMF ends up storing in
            // `self->param`, and only the first is a straight copy:
            //   * core/ControlMap.cpp's EnqueueCast writes
            //     `op.param.form = req->spell` and `op.param.ival = req->flags`
            //     into a zero-initialised APMF_Param, and nothing else.
            //   * core/ControlMap.cpp's ApplyRequest then REWRITES that form
            //     before the claim is stored, in exactly two cases:
            //       - kCastFlag_DenyHandOnly -> forced to 0 (that claim drives
            //         NOTHING for its whole life);
            //       - kCastFlag_FromPackage  -> replaced by the spell APMF
            //         EXTRACTED from the package.
            // MFO sets NEITHER flag today, which is the only reason a plain
            // `form = wantSpell` was byte-exact -- an unrecorded dependency, and
            // MFO's own deny-only hand claim is queued work
            // (memory cast-deny-only-hand-claim / apmf-f3-denyhand-followups).
            // So the mint below mirrors the DenyHandOnly rewrite locally. It
            // CANNOT mirror the FromPackage one: the extracted spell is resolved
            // inside APMF and there is no query that hands it back, so a future
            // caller that sets kCastFlag_FromPackage MUST solve that first (an
            // APMF-side read of the resolved spell) rather than assume this copy
            // is still exact. Stated here so the next reader inherits the
            // dependency instead of re-deriving it from a warn flood.
            //
            // WHAT GOES WRONG IF THE MIRROR DRIFTS. Not a mask -- ApplyRepoint
            // still renews the TTL -- but it REFUSES the form change and says so
            // (`spdlog::warn`), so every heartbeat would emit one warn per claim
            // per interval (~3 s at the defaults) for as long as the claim stands.
            //
            // WHY VERBATIM AND NOT REBUILT. APMF's ApplyRepoint REFUSES (loudly)
            // a Repoint whose `param.form` differs from the claim's current
            // spell, because a cast claim's delivery-flip proxy, resolved target
            // handle and CastFlags were all resolved AGAINST the original spell
            // inside RequestCast and Repoint re-runs none of that -- honouring a
            // form swap would leave the claim NAMED for one spell while DRIVING
            // another's proxy at another's target. A stored copy cannot drift
            // from what was requested; a rebuild can (the flags word is derived
            // from hand/conc/stopPct through three conditionals, and any future
            // edit to that derivation would silently desync the heartbeat).
            APMF_API::APMF_Param reqParam{};
        };

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
            // -- PER-CAST, TTL-bounded, single slot (LEFT always -- ClaimHealCast's
            // own hard rule, never per-hand). Held by ComposedCast for the life of
            // a claimed heal; refreshed every tick the gambit still wants it
            // (create-or-refresh on a spell/target/hand/concentration/stopPct
            // change -- RequestCast has no in-place re-point, so a CHANGE releases
            // and re-requests, a new bounded claim), released the instant it stops
            // (ComposedCast::End), and auto-expired by FacetExpiry() (below) AND
            // (on APMF's side) by the claim's own TTL if a caller forgets. Distinct
            // from `offense` below (heal and offense claims never overlap on one
            // follower's SAME hand -- CasterConsent::SpellKind makes them mutually
            // exclusive per tick for a given spell -- but each gets its own state
            // to avoid any cross-talk; a heal on LEFT and an offense claim on RIGHT
            // CAN now be concurrently live, feat/per-hand-cast-slots).
            CastClaim heal;
            // offense-cast (ch.8b, kIntent_Cast/RequestCast, PORTED feat/offense-
            // cast-seats, 2026-09-05, off the retired ch.8 kIntent_SelectSpell
            // gate-only claim this slot used to back -- see APMFBridge.h's
            // ClaimOffenseCast doc) -- PER-CAST, TTL-bounded, and PER-HAND
            // (feat/per-hand-cast-slots, 2026-09-06): [0] = left, [1] = right, so
            // two independent offense claims can stand on one follower at once
            // (the parallel APMF change arbitrates kIntent_Cast per (actor, hand)
            // now). A DualCast claim is ONE underlying handle mirrored into BOTH
            // indices -- see ClaimOffenseCast's own doc for how that mirroring is
            // created and torn down without a double-release.
            CastClaim offense[2];
            // ── THE IDLE-HAND FLOOR (F10, marth's design ruling 2026-09-08) ──
            // A SECOND, DENY-ONLY kIntent_Cast claim (APMF_API::
            // kCastFlag_DenyHandOnly) standing on whichever ONE hand MFO's own
            // driving claim above does NOT occupy.
            //
            // WHY MFO CLAIMS IT AT ALL. APMF scopes a cast claim's deny to that
            // claim's own hand, and leaving the other hand permissive is CORRECT
            // for a general framework -- an unclaimed hand belongs to the actor's
            // own AI. MFO's design is the narrower one: only GAMBITED spells
            // occur. marth, 2026-09-08: *"The followers other hand is supposed to
            // be claimed by MFO as idle is it not? Its proper APMF for it to
            // behave like it is currently. But its improper MFO. Under MFO it
            // should be idle when unclaimed."* So "the other hand goes idle" is
            // the SPECIFICATION here, not a cost being traded against the casts
            // it forgoes. Field shape it closes (Docs/DIAG-2026-09-08-field.md
            // RC3): every un-gambited cast that session -- 2 Poison Sprays, 5
            // Stone Runes, 3 Raise Zombies -- charged on the RIGHT hand inside a
            // window where MFO held only a LEFT-hand claim; where a claim DID
            // occupy the hand the deny went 3 for 3.
            //
            // ONLY EVER A COMPANION TO A DRIVING CLAIM -- never a standing hold
            // of its own. See ReconcileHandFloorLocked below for the full
            // derivation, including WHY "MFO holds no cast claim at all" floors
            // NEITHER hand rather than both.
            //
            // Reuses CastClaim wholesale so it inherits the same TTL heartbeat,
            // liveness check, never-published fail-closed split and reqParam
            // mirror as every other cast claim in this file -- one mechanism, not
            // a second one (its `spell` stays 0 and its `denyOnly` stays true for
            // the claim's whole life).
            CastClaim floor;
        };
        std::mutex                             g_mx;
        std::unordered_map<RE::FormID, Owned>  g_owned;

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

        // Release ONE CastClaim's handle (if live) and reset it to default.
        // Caller holds g_mx. Safe to call on an already-empty claim (no-op).
        void ReleaseClaimLocked(CastClaim& c) {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);
            c = CastClaim{};
        }

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
                                   bool wantDenyOnly = false) {
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

        // Drop the map entry once EVERY claim is gone. Caller holds g_mx.
        void EraseIfEmpty(std::unordered_map<RE::FormID, Owned>::iterator it) {
            const auto& o = it->second;
            if (o.targetHandle == APMF_API::kInvalidHandle &&
                o.packageHandle == APMF_API::kInvalidHandle && o.actionHandle == APMF_API::kInvalidHandle &&
                o.equipHandle == APMF_API::kInvalidHandle && o.heal.handle == APMF_API::kInvalidHandle &&
                o.offense[0].handle == APMF_API::kInvalidHandle &&
                o.offense[1].handle == APMF_API::kInvalidHandle &&
                // F10: an entry holding ONLY the idle-hand floor must survive --
                // erasing it would drop MFO's record of a claim that is still
                // standing on APMF's side, leaking it until its TTL. It cannot
                // normally happen (the floor is released in the same Tick() pass
                // that sees the last driving claim go), but a caller-driven
                // release ordering must not be able to create it.
                o.floor.handle == APMF_API::kInvalidHandle)
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

    namespace {
        // Left -> slot 0; everything else (kApmfHandRight, bare 0, or an
        // unrecognized value) -> slot 1. Dual is handled separately by its own
        // caller (ClaimOffenseCast mirrors into BOTH slots; a single-slot
        // lookup is meaningless for it, so nothing here maps kApmfHandDualCast
        // specially -- callers never pass it to this helper).
        inline std::size_t OffenseSlot(std::int32_t a_hand) {
            return a_hand == kApmfHandLeft ? 0 : 1;
        }
    }

    bool IsOwnedCastActive(RE::FormID a_follower) {
        // Fast-out before the lock: APMF absent -> never active (mirrors every
        // other accessor's g_apmf check).
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() &&
               (it->second.offense[0].handle != APMF_API::kInvalidHandle ||
                it->second.offense[1].handle != APMF_API::kInvalidHandle);
    }

    bool IsOwnedCastActiveOnHand(RE::FormID a_follower, std::int32_t a_hand) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() &&
               it->second.offense[OffenseSlot(a_hand)].handle != APMF_API::kInvalidHandle;
    }

    RE::FormID GetOffenseCastProxy(RE::FormID a_follower, std::int32_t a_hand) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return 0;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return 0;
        const auto& c = it->second.offense[OffenseSlot(a_hand)];
        return c.handle != APMF_API::kInvalidHandle ? c.proxy : 0;
    }

    // ── offense-cast (per-cast, TTL-bounded, PER-HAND, ch.8b, kIntent_Cast/RequestCast) ──
    // PORTED feat/offense-cast-seats (2026-09-05) off the retired ch.8
    // kIntent_SelectSpell gate-only claim (ClaimCasting/ReleaseCasting, removed)
    // this call site used to make; REKEYED per-hand (feat/per-hand-cast-slots,
    // 2026-09-06) -- see APMFBridge.h's ClaimOffenseCast doc for the full
    // rationale. Claim plumbing, mirroring ClaimHealCast's shape via the shared
    // EnsureCastClaimLocked helper, just applied to ONE of the two `offense[]`
    // slots (or BOTH, mirrored, for a DualCast ask).
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

        if (a_hand == kApmfHandDualCast) {
            // Defensive: if slot 1 (right) currently holds an INDEPENDENT
            // (non-mirrored) single-hand claim, release it BEFORE mirroring
            // the dual claim over it below -- overwriting a live handle
            // without releasing it would leak an APMF-side claim. Callers are
            // expected to have already verified BOTH hands are free via the
            // per-hand cast-lock juggle (Actuation.cpp's ResolveCastHand)
            // before ever requesting Dual, so this should be a no-op in
            // practice; it exists so a caller mistake corrupts nothing.
            if (o.offense[1].handle != APMF_API::kInvalidHandle &&
                o.offense[1].handle != o.offense[0].handle)
                ReleaseClaimLocked(o.offense[1]);
            // ONE underlying APMF claim (a single RequestCast with
            // kCastFlag_DualCast) occupies BOTH hands -- mirror it into both
            // slots rather than asking APMF to arbitrate the same dual claim
            // twice. Ensure against slot 0 (the "primary"), then copy its
            // result into slot 1 verbatim (same handle -- see ReleaseOffenseCast
            // for the matching dedup-on-release).
            EnsureCastClaimLocked(v5, a_follower, o.offense[0], a_spell, a_target, a_hand,
                                  a_concentration, a_stopPct);
            o.offense[0].refreshed = std::chrono::steady_clock::now();
            o.offense[1] = o.offense[0];
        } else {
            const std::size_t idx   = OffenseSlot(a_hand);
            const std::size_t other = 1 - idx;
            // If the OTHER slot currently mirrors a live DUAL claim (same
            // handle this single-hand ask is about to replace), un-mirror it
            // FIRST -- Ensure below will Release/replace slot `idx`'s copy of
            // that shared handle, and the other slot's copy must not go on
            // pointing at a handle that no longer exists.
            if (o.offense[idx].handle != APMF_API::kInvalidHandle &&
                o.offense[idx].handle == o.offense[other].handle)
                o.offense[other] = CastClaim{};
            EnsureCastClaimLocked(v5, a_follower, o.offense[idx], a_spell, a_target, a_hand,
                                  a_concentration, a_stopPct);
            o.offense[idx].refreshed = std::chrono::steady_clock::now();
        }

        const bool live = a_hand == kApmfHandDualCast
            ? o.offense[0].handle != APMF_API::kInvalidHandle
            : o.offense[OffenseSlot(a_hand)].handle != APMF_API::kInvalidHandle;
        EraseIfEmpty(g_owned.find(a_follower));
        return live;
    }

    // See APMFBridge.h. Kept RIGHT BESIDE ClaimOffenseCast on purpose: this is
    // that function's own non-arbitration early-return set, and nothing else --
    // if one ever gains a condition, the other is one screen away.
    bool OffenseCastClaimSupported() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= 5 && Config::g_apmfCast.load();
    }

    // ── REFRESH A STANDING CAST CLAIM IN PLACE (F9, 2026-09-08) ─────────────────
    // See APMFBridge.h for the contract. The implementation is deliberately a
    // REPLAY of the claim's OWN stored tuple rather than a second refresh path:
    // handing EnsureCastClaimLocked exactly what it already stored guarantees the
    // identity compare MATCHES, so this can never change a hand mode, never
    // re-point a target, and never tear down a claim that is still in force -- the
    // things F8 exists to stop. Everything the fast path does (the ABI-6 liveness
    // check, the lazy proxy read, the TTL heartbeat, the never-published
    // fail-closed split) happens here too, for free and identically, because it is
    // the same code.
    //
    // "THE FAST PATH IS THE ONLY PATH" WOULD BE FALSE, so it is not claimed
    // (review fix, 2026-09-08). The identity compare matching only decides that the
    // request is UNCHANGED; what happens next still depends on APMF. If the stored
    // handle reads NOT live and had once been live, the aged-out branch runs and
    // RE-REQUESTS -- a genuinely fresh handle is minted from here. That is correct
    // and is the point (a claim APMF expired should come back while its rule still
    // wants it), and it is NOT a concurrent second claim: the dead handle is
    // dropped first, exactly one claim exists at every instant, and no charge is
    // interrupted because there was no live claim left to interrupt. The two
    // outcomes a caller must distinguish stay as documented: `true` = a live claim
    // stands on that hand afterwards (renewed or re-minted), `false` = APMF refused
    // it and nothing stands.
    //
    // IT DOES NOT COLLIDE WITH RefreshHealCastClaim's DELIBERATE NON-RENEWAL.
    // That function refuses to Repoint on purpose, because it is ComposedCast's
    // incumbent HOLD -- kept alive for a claim whose own rule may have gone silent
    // -- and renewing APMF's window from there would remove the liveness bound the
    // hold depends on. This function is the opposite case and the one the
    // heartbeat was always scoped to: it is called ONLY by a rule that WON its lap
    // and is asking for the identical claim it already holds. Same precondition,
    // same renewal, no new hold.
    //
    // IT DOES NOT MOVE `created` EITHER (the never-observed hold cap's clock):
    // that is stamped once per handle in the mint block, which this cannot reach.
    bool RefreshOwnedCastOnHand(RE::FormID a_follower, std::int32_t a_hand) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || api->abiVersion < 5) return false;
        auto* v5 = reinterpret_cast<const APMF_API::APMF_API_v5*>(api);
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return false;
        auto& o = it->second;
        const auto now = std::chrono::steady_clock::now();
        bool any = false;

        auto replay = [&](CastClaim& c) {
            if (c.handle == APMF_API::kInvalidHandle) return;
            EnsureCastClaimLocked(v5, a_follower, c, c.spell, c.target, c.hand,
                                  c.conc, c.stopPct, c.denyOnly);
            c.refreshed = now;
            if (c.handle != APMF_API::kInvalidHandle) any = true;
        };
        // A mirrored DualCast claim is ONE handle living in BOTH slots, and BOTH
        // copies' `refreshed` stamps have to move together: Tick()'s sweep reads
        // each slot independently, so a stale mirror would release the shared
        // handle out from under the live slot (and then clear it, via that
        // sweep's own dual dedup). Re-mirror after the replay, exactly as
        // ClaimOffenseCast does.
        auto replayOffense = [&](std::size_t idx) {
            auto& c = o.offense[idx];
            if (c.handle == APMF_API::kInvalidHandle) return;
            const std::size_t other = 1 - idx;
            const bool sharedDual = o.offense[other].handle == c.handle;
            replay(c);
            if (sharedDual) o.offense[other] = c;
        };

        if (a_hand == kApmfHandDualCast) {
            replayOffense(0);
            // "Both hands" is USUALLY one mirrored dual claim, which replayOffense
            // has already re-mirrored -- but the caller names HANDS, not claims,
            // and two INDEPENDENT single-hand claims can also occupy both. Refresh
            // the second one too when it is a genuinely different handle, or its
            // `refreshed` stamp would stand still and Tick()'s sweep would release
            // a claim this call was asked to keep alive.
            if (o.offense[1].handle != APMF_API::kInvalidHandle &&
                o.offense[1].handle != o.offense[0].handle)
                replayOffense(1);
        } else if (a_hand == kApmfHandLeft) {
            replayOffense(0);
            // Heals are LEFT always (ClaimHealCast's hard rule), and a heal claim
            // and an offense claim CAN both stand on the left hand at once, so
            // both are refreshed -- the caller named the hand, not the facet.
            replay(o.heal);
        } else {
            replayOffense(1);
        }

        EraseIfEmpty(g_owned.find(a_follower));
        return any;
    }

    // ── PER-HAND CAST-CLAIM RELEASE (rank preemption, marth 2026-09-08) ─────────
    // See APMFBridge.h. Exists because ReleaseOffenseCast is whole-follower by
    // design (its two callers both mean "nothing wanted on either hand anymore")
    // and rank preemption means exactly the opposite: a higher-ranked rule takes
    // ONE hand, and the other hand's independent claim must not even notice.
    //
    // A MIRRORED DUALCAST CLAIM IS ONE CLAIM ON TWO HANDS, so releasing "its left
    // hand" releases the whole thing and clears both slots. That is the honest
    // semantics, not a shortcut: there is one APMF handle, APMF arbitrates it as
    // one claim occupying both hands, and there is no half-release to make. A
    // caller preempting one hand of a dual cast is ending that dual cast.
    void ReleaseCastClaimOnHand(RE::FormID a_follower, std::int32_t a_hand) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        auto& o = it->second;
        const std::size_t idx   = OffenseSlot(a_hand);
        const std::size_t other = 1 - idx;
        if (o.offense[idx].handle != APMF_API::kInvalidHandle) {
            const bool sharedDual = o.offense[other].handle == o.offense[idx].handle;
            ReleaseClaimLocked(o.offense[idx]);
            if (sharedDual) o.offense[other] = CastClaim{};   // one handle, released once
        }
        // Heals are LEFT always (ClaimHealCast's own hard rule), so the heal slot
        // is only ever part of the LEFT hand.
        if (idx == 0) ReleaseClaimLocked(o.heal);
        EraseIfEmpty(it);
    }

    void ReleaseOffenseCast(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        auto& o = it->second;
        // A mirrored DualCast claim shares ONE handle across both slots --
        // release it exactly once (Release on an already-cleared/invalid
        // handle would be a no-op anyway, but this avoids depending on that
        // and keeps the intent explicit: one claim, one Release call).
        const bool sharedDual = o.offense[0].handle != APMF_API::kInvalidHandle &&
                                 o.offense[0].handle == o.offense[1].handle;
        ReleaseClaimLocked(o.offense[0]);
        if (sharedDual) o.offense[1] = CastClaim{};
        else            ReleaseClaimLocked(o.offense[1]);
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
        EnsureCastClaimLocked(v5, a_follower, o.heal, a_spell, a_target, a_hand,
                              a_concentration, a_stopPct);
        o.heal.refreshed = std::chrono::steady_clock::now();
        const bool live = o.heal.handle != APMF_API::kInvalidHandle;
        EraseIfEmpty(g_owned.find(a_follower));
        return live;
    }

    // See APMFBridge.h. The heal twin of OffenseCastClaimSupported, kept beside
    // ClaimHealCast for the same reason -- it mirrors THAT function's own
    // non-arbitration early returns (toggle = bHealAnimPackage, not bApmfCast).
    bool HealCastClaimSupported() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= 5 && Config::g_healAnimPackage.load();
    }

    void ReleaseHealCast(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseClaimLocked(it->second.heal);
        EraseIfEmpty(it);
    }

    bool IsHealCastActive(RE::FormID a_follower) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() && it->second.heal.handle != APMF_API::kInvalidHandle;
    }

    RE::FormID GetHealCastProxy(RE::FormID a_follower) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return 0;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        if (it == g_owned.end() || it->second.heal.handle == APMF_API::kInvalidHandle) return 0;
        return it->second.heal.proxy;
    }

    // See the header for the full rationale (why the stamp, why the age cap, and
    // why the ABI < 6 branch refuses outright). Deliberately does NOT release a
    // dead claim: the SAME shape EnsureCastClaimLocked uses for a handle APMF has
    // already auto-expired -- stop treating it as live and let the normal path
    // (here: Tick()'s sweep, which then sees a stamp that stopped moving) clear
    // it, rather than issuing a Release against a handle APMF no longer knows.
    bool RefreshHealCastClaim(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end() || it->second.heal.handle == APMF_API::kInvalidHandle) return false;
        // ── THE NEVER-OBSERVED HOLD CAP (Fable SEV-1 2026-09-06; MECHANISM AND
        //    SIZING BOTH CORRECTED by the Fable diff review, same day) ─────────
        // The ONLY caller is ComposedCast::Try's incumbent hold, and it calls
        // this only while the incumbent has NOT been observed firing.
        //
        // THE RENEWAL MECHANISM IS REAL NOW -- AND THIS FUNCTION STILL MUST NOT
        // USE IT (F5-1, 2026-09-08). The note that stood here said the F2 renewal
        // could not happen at all ("MFO never Repoints a kIntent_Cast claim, and
        // APMF main's ApplyRepoint never touches expiresMs; F2 is on an unmerged
        // branch"). Both halves are now false: APMF's TTL RENEWAL is merged
        // (core/ControlMap.cpp -- a Repoint on a LIVE cast claim moves its
        // deadline to now + its own granted ttlMs), and EnsureCastClaimLocked's
        // fast path Repoints a live cast claim on a heartbeat. Rewritten rather
        // than deleted, because a comment teaching a false mechanism is worse
        // than no comment.
        //
        // WHAT THAT MEANS HERE. The old note's fear -- a renewal keeping
        // IsClaimLive true forever and deadlocking this hold -- is exactly why
        // the heartbeat lives in EnsureCastClaimLocked and NOT in this function.
        // A renewal there is sent only by a rule that WON its lap and is asking
        // for the identical claim it already holds; this function is the opposite
        // case, a hold kept alive for an incumbent whose own rule may have gone
        // silent. So this function still only bumps MFO's local `refreshed` and
        // NEVER Repoints: the moment the incumbent's rule stops asking, nothing
        // renews APMF's window and the claim dies at its TTL, bound 1 intact.
        // Bound 2 (below) actually gets STRONGER, because `created` is no longer
        // reset by a 6s auto-expiry + re-request cycle that no longer happens.
        //
        // WHAT THE CAP REALLY DOES, and why it is still needed. It stops the
        // HOLD'S HEARTBEAT from outliving the incumbent RULE's interest. Every
        // other exit the hold has is conditional on something that may never
        // happen: `observed` needs the engine to really cast; an incumbent CHANGE
        // needs a successful ClaimHealCast the hold itself prevents (and
        // ComposedCast::End() has no caller at all); Tick()'s FacetExpiry sweep
        // cannot fire while this very function keeps bumping `refreshed`. And the
        // incumbent's own rule keeps ClaimHealCast-ing for as long as its
        // condition holds, re-requesting a fresh handle whenever APMF expires the
        // old one. So an incumbent the engine NEVER casts can re-arm indefinitely
        // while a competing rule starves behind it.
        //
        // SIZED FROM THE MEASURED HEAL LATENCY, NOT FROM THE CLAIM TTL AND NOT
        // FROM AN OFFENSE FIRE (#9). kHealHoldNeverObservedMs is 4000ms, sized
        // from the claim-to-OBSERVED time of the one heal that landed on the deck
        // (2.95s: minted 19:59:11.158, MFO `[cast]` 19:59:14.110) plus the
        // measured tail. It is NOT kHealCastTtlMs's 6000ms (what this line read
        // before the Fable diff review), and it is NOT the 2.3-2.5s equip+charge
        // number that briefly sized it at 3000ms -- that figure is an OFFENSE
        // Firebolt, and lifting at 3000ms would have released a heal that was
        // already CASTING. See kHealHoldNeverObservedMs's own comment in
        // APMFBridge.h for the full working, the lap-granularity slack, and the
        // trade-off it is chosen on. Note there is no "provably dead" point in
        // this evidence, and the reachable-hold cases are TWO, not one (a held
        // rule usually outranks the incumbent, but a lower-ranked rule is held
        // whenever the incumbent's own condition has gone false).
        //
        // `created` is stamped once per handle, in EnsureCastClaimLocked's mint
        // block right after its RequestCast, and is moved by no refresh, no
        // APMF-side liveness check, not by this heartbeat and not by F5-1's TTL
        // heartbeat (which moves `renewed` instead, precisely so this clock stays
        // still) -- but it IS moved when the incumbent's own rule re-requests
        // after an APMF auto-expiry (that function's `everLive` aged-out branch
        // falls through to the same mint, giving a new handle a new stamp; F5-1
        // makes that path RARE while a rule keeps winning, since the window is
        // renewed instead of being allowed to lapse).
        // That is harmless HERE, and only here, because that path cannot run
        // underneath a live hold: the incumbent's re-request returns Claimed,
        // which stops the rule scan before any held rule's Try() is reached. It
        // is not the blanket immunity this comment used to assert.
        //
        // NOT a cap on an OBSERVED claim: a live channelled heal never routes
        // through here at all (the caller's `!observed` guard), so a real cast
        // keeps running for as long as its rule and APMF agree it should.
        const auto now = std::chrono::steady_clock::now();
        if (now - it->second.heal.created >= std::chrono::milliseconds(kHealHoldNeverObservedMs))
            return false;
        // ABI < 6 has no IsClaimLive, so there is NO bound available here at all:
        // the unchanged fast path in EnsureCastClaimLocked trusts a stored handle
        // forever on ABI < 6, so the incumbent's own rule keeps `refreshed`
        // moving and Tick()'s sweep never fires either. Reporting the stored
        // handle as live would therefore hold a newcomer off behind a handle APMF
        // may have silently expired, with nothing to end it (#7 -- that is a mask,
        // and the header used to claim a sweep bound it does not have). Refuse:
        // an honest degrade to the pre-F1 behaviour (both rules thrash the slot,
        // visibly), never a hold nobody can break. Inert in practice -- the pair
        // ships together at kABIVersion 6.
        if (api->abiVersion < 6) return false;
        if (!reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->IsClaimLive(it->second.heal.handle))
            return false;                       // dead APMF-side: no heartbeat, Tick() sweeps it
        it->second.heal.refreshed = now;
        // LATCH THE OBSERVATION TOO (SEV-4, pre-merge review 2026-09-07). This IS a
        // genuine `IsClaimLive == true` sighting of this exact handle, and everLive's
        // own doc says it is set "the first time IsClaimLive(handle) answers true".
        // Without it, a heal whose only liveness sightings came through the hold path
        // would still look never-published to EnsureCastClaimLocked and be reported as
        // an outright refusal it never suffered.
        it->second.heal.everLive = true;
        return true;
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
            // offense[0]/[1]: PER-HAND now (feat/per-hand-cast-slots) -- each
            // slot expires independently. A mirrored DualCast claim shares ONE
            // handle across both slots and keeps identical `refreshed` stamps
            // (ClaimOffenseCast stamps both together), so it goes stale on the
            // SAME sweep pass for both -- release the shared handle exactly
            // once (capture the dual-ness BEFORE releasing slot i, since
            // releasing zeroes it).
            for (std::size_t i = 0; i < 2; ++i) {
                auto& c = o.offense[i];
                if (c.handle != APMF_API::kInvalidHandle && now - c.refreshed >= facetExpiry) {
                    const bool sharedDual = o.offense[1 - i].handle == c.handle;
                    ReleaseClaimLocked(c);
                    if (sharedDual) o.offense[1 - i] = CastClaim{};
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
            if (o.heal.handle != APMF_API::kInvalidHandle && now - o.heal.refreshed >= facetExpiry)
                ReleaseClaimLocked(o.heal);
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
            if (o.targetHandle == APMF_API::kInvalidHandle &&
                o.packageHandle == APMF_API::kInvalidHandle && o.actionHandle == APMF_API::kInvalidHandle &&
                o.equipHandle == APMF_API::kInvalidHandle && o.heal.handle == APMF_API::kInvalidHandle &&
                o.offense[0].handle == APMF_API::kInvalidHandle &&
                o.offense[1].handle == APMF_API::kInvalidHandle &&
                o.floor.handle == APMF_API::kInvalidHandle)
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
        }
        g_owned.clear();
    }
}
