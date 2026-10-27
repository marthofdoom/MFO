#include "PCH.h"
#include "Followers.h"
#include "Loadout.h"
#include "Targeting.h"
#include "EngageOnSight.h"   // ReleaseHeldState: forget the engage-on-sight entry
#include "CasterConsent.h"   // v1.0.30: dismissal releases the cast latch too
#include "CombatStyle.h"     // v1.0.33: dismissal drops weapon-stance ownership
#include "cast/Actuation.h"       // T#76: dismissal releases the equip force-hold too
#include "Packages.h"
#include "apmf/APMFBridge.h"   // ReleaseHealCast/ReleaseOffenseCast -- drop a dismissed follower's live claims
#include "ComposedCast.h" // ClearWatch -- drop the shared [cfc] silent-claim watch alongside the offense claim
#include "CastBounds.h"   // Disarm -- drop his MFO-executed-cast bound (§2 registry)
#include "logistics/Logistics.h"
#include "MainThread.h"   // F7: the deferred dismissal restore under the APMF equip authority
#include "Diagnostics.h"  // F7: CurrentPumpEpoch / PumpTickGate for its AddTask body
#include "progression/ProgAllocator.h"   // 86e3eewaf: ForgetFollower (the main-thread progression undo)
#include "logistics/PlayerGiven.h"        // 86e3eewaf: Forget (his PGIV entries)
#include "FollowerCatchUp.h"               // Poll: the left-behind catch-up
#include "Rapport.h"                      // review F5: DrainPendingSpends (the respec debit, worker side)
#include <mutex>                          // 86e3eewaf: g_rosterMx (main -> worker verdict hand-back)
#include "Forms.h"
#include "Config.h"
#include "Vocabulary.h"
#include <unordered_set>   // SEV-1: off-worker FormID membership mirror (g_tracked)

namespace MFO::Followers {

    // EVERY follower's base gambits. Applied when a record is first created
    // (TryEnsureRecord) AND as a backfill for a record that loads with BOTH
    // tables empty -- a pre-defaults save, where the co-save load assigns
    // g_followers[] directly and would otherwise leave the board permanently
    // blank. NOT seeding-behind-a-test-flag (marth: nothing should be seeded
    // that can't be manually set) -- every op here is in the board's pickers,
    // so the player can edit, reorder or delete any of it. Sized to the Rank I
    // slots (3 combat, 4 logistics) so a co-save round-trip never truncates it
    // (#11). A record that loads with ANY rule is left exactly as saved.
    void ApplyDefaultKit(FollowerState& st) {
        auto add = [](std::vector<Gambit>& tab, const char* cond, float p, const char* act) {
            Gambit g{}; g.conditionOpcode = cond; g.conditionParam = p; g.actionOpcode = act;
            tab.push_back(g);
        };
        add(st.combat(), Vocab::kCondSelfHpBelow, 0.30f, Vocab::kActDrinkHealthPotion); // survive: drink when hurt
        add(st.combat(), Vocab::kCondFoeLowestHp, 0.0f,  Vocab::kActAttack);  // else fight the weakest foe
        add(st.combat(), Vocab::kCondAlways,      0.0f,  Vocab::kActWait);     // otherwise hold
        add(st.logistics(), Vocab::kCondSelfHpBelow,          0.50f, Vocab::kActDrinkHealthPotion);
        add(st.logistics(), Vocab::kCondSelfOutOfArrows,      10.0f, Vocab::kActLootArrows);
        add(st.logistics(), Vocab::kCondSelfLowHealthPotion,   2.0f, Vocab::kActLootPotions);
        add(st.logistics(), Vocab::kCondAlways,               0.0f,  Vocab::kActLootEquipment);
    }

    namespace {

        // ── quirk table (data/follower_quirks.json, hand-compiled) ──────────
        // Kept as code rather than parsed at runtime: no file paths to break
        // in-game, no parser dependency, and the table cannot drift from the
        // code consuming it (ARCHITECTURE.md §8.2).

        struct FactionQuirk {
            const char* name;
            const char* plugin;
            RE::FormID  localID;
            std::int32_t rank;          // -1 = "membership alone means dismissed"
            RE::TESFaction* resolved = nullptr;
        };

        // Inigo: sets PlayerTeammate but does NOT reliably clear it on
        // dismissal (DESIGN.md §3.1, corrected 2026-07-21). THE case the first
        // design draft got wrong -- the draft claimed he never sets it at all,
        // which is why the quirk AV below, not the teammate flag, is decisive.
        constexpr float kInigoDismissedAV = -1.0f;

        int g_quirksActive = 0, g_quirksInactive = 0;

        // Last time each follower was SEEN fighting. Sampled on the sweep, not
        // read at death time -- see SecondsSinceCombat's note (#51).
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_lastCombat;

        // Consecutive sweeps a follower may be missing before we believe it.
        // FIELD-FOUND 2026-07-21: the log showed `- id` then `+ id` 117ms apart,
        // i.e. a transient miss -- and because the death sink Refreshes before
        // awarding, a kill landing in that window credited NOBODY. One sweep is
        // not evidence of absence.
        constexpr int kMissesBeforeDrop = 3;
        std::unordered_map<RE::FormID, int> g_missStreak;

        // CLAIM THROUGH LOADS (marth 2026-10-19: "Keep the claim through loads"; ClickUp 86e3qmmef + the
        // 10-18 load-gap finding). At every load screen the sweep drops a follower for 5-40 s (Sven loads
        // later than Auri: `- id` then `+ id` seconds apart, always right after a main-thread stall) and the
        // dismissal release handed his ch.17 equip claim back -- and in that gap the engine stripped worn
        // gear (a declared ring, 3 of 4 armor pieces) until the re-claim re-declared. A drop that happens
        // within kLoadWindowS of a world transition (LoadingMenu seen, a worker gap, a player interior/
        // worldspace change or a position jump) while the actor is STILL an eligible teammate is TRANSIENT:
        // he leaves g_active as before but ReleaseHeldState is NOT run, so the claim and the change
        // detector stay standing; Refresh finishes the release if he is still absent after kTransientHoldS
        // or stops being eligible. A drop with no transition in sight releases exactly as before.
        // Worker/serial-pump only, like every map here; cleared with them.
        constexpr float kLoadWindowS     = 60.0f;   // Sven's measured absence after a load: up to ~40 s
        constexpr float kTransientHoldS  = 60.0f;
        constexpr float kRefreshGapS     = 2.5f;    // Refresh runs every kDiagEveryNth=4 pump wakes (~532 ms, Diagnostics.cpp:68); a longer gap = the worker was stalled by a load
        constexpr float kTeleportJumpU   = 4096.0f; // player position jump between sweeps = fast travel / cell load
        using Clock = std::chrono::steady_clock;
        Clock::time_point                             g_lastRefreshAt{};
        Clock::time_point                             g_transitionAt{};
        std::atomic<long long>                        g_transitionNs{ 0 };   // worker-readable mirror of g_transitionAt (SecondsSinceTransition)
        const void*                                   g_lastPlaceKey = nullptr;
        RE::NiPoint3                                  g_lastPcPos{};
        bool                                          g_havePcPos = false;
        std::unordered_map<RE::FormID, Clock::time_point> g_transientDrop;   // id -> when the transient drop began

        // [follower] STATE LINE (2026-09-21, Deck/Fable: Jesper stood still for
        // three minutes on an ACTIVE PlayerFollowerPackage and nothing MFO
        // printed could tell Follow from Wait -- both are the same package
        // form, branched on the WaitingForPlayer actor value). Printed on the
        // `+` roster line and then every ~kStateLineS per active follower from
        // the worker sweep:
        //   [follower] <id> '<name>' pkg=<current package form> waitingForPlayer=<AV 95>
        //              dPlayer=<u> moved=<u since the last line> aiEnabled=<kProcessMe> inCombat=<0/1>
        // Pure reads (GetCurrentPackage -> currentProcess->GetRunningPackage,
        // AsActorValueOwner()->GetActorValue, IsAIEnabled = boolBits kProcessMe;
        // pinned 3.7.0 Actor.h:533/:570, ActorValues.h:103). Worker/serial-pump
        // domain like every map above; cleared with them.
        constexpr float kStateLineS = 30.0f;
        struct StateSample {
            std::chrono::steady_clock::time_point at{};
            RE::NiPoint3                          pos{};
        };
        std::unordered_map<RE::FormID, StateSample> g_stateSample;

        void LogStateLine(RE::Actor* a_actor, RE::FormID a_id, const char* a_why) {
            auto&       smp    = g_stateSample[a_id];
            const auto  now    = std::chrono::steady_clock::now();
            const auto  pos    = a_actor->GetPosition();
            const float moved  = (smp.at.time_since_epoch().count() != 0) ? pos.GetDistance(smp.pos) : 0.0f;
            const auto* pkg    = a_actor->GetCurrentPackage();
            const float wait   = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWaitingForPlayer);
            float       dPlayer = -1.0f;
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) dPlayer = pos.GetDistance(pc->GetPosition());
            spdlog::info("[follower] {:08X} '{}' pkg={:08X} waitingForPlayer={:.0f} dPlayer={:.0f} moved={:.0f} aiEnabled={} inCombat={} [{}]",
                         a_id, a_actor->GetName() ? a_actor->GetName() : "?",
                         pkg ? pkg->GetFormID() : 0u, wait, dPlayer, moved,
                         a_actor->IsAIEnabled() ? 1 : 0, a_actor->IsInCombat() ? 1 : 0, a_why);
            smp.at  = now;
            smp.pos = pos;
        }

        // ── off-worker membership mirror + active snapshot (SEV-1) ──────────
        // g_active / g_activeIds are rebuilt by Refresh on the JOB WORKER
        // (ENGINE_NOTES §0.37) and are read UNLOCKED by main/worker callers.
        // Off-worker callers (combat cast-hook, event sinks, SaveCallback,
        // progression poll) must NOT walk those live lists -- Refresh
        // reassigns/reallocates them concurrently (UAF/rehash). g_mx guards the
        // two structures BELOW only; it never wraps g_active itself (keeps the
        // serial pump domain lock-free, #4). Refresh republishes both under g_mx
        // whenever it rebuilds the lists, and ClearTransientState empties them
        // under the same lock on revert.
        std::mutex                                       g_mx;
        std::unordered_set<RE::FormID>                   g_tracked;   // FormID membership mirror
        std::unordered_set<RE::FormID>                   g_mfoOff;    // T#78 mfoEnabled==false mirror
        // The RESOLVED class (1/2/3, Logistics::ResolveBaseClass) per follower, for
        // the base HMS split on the progression poll's TRUE main thread (#74: never
        // the live map). Holds every record with a picked class, plus every ACTIVE
        // follower on Auto (resolved by the vote). Absent = 0 = unresolved.
        std::unordered_map<RE::FormID, std::uint8_t>     g_resolvedClass;
        std::shared_ptr<const std::vector<RE::FormID>>   g_activeSnapshot =
            std::make_shared<const std::vector<RE::FormID>>();

        // Republish the mirror + snapshot from the current g_activeIds. Call
        // under NOTHING (it takes g_mx itself); invoked at Refresh's tail, from
        // ClearTransientState and (via RepublishActiveMirror) the Board's mfoEnabled write. g_mx is a strict LEAF -- no MFO call is made
        // while it is held.
        void PublishActiveMirror() {
            // Resolve the classes BEFORE taking g_mx (it is a strict leaf: the
            // resolver makes engine reads). Worker / drained-pump domain, the same
            // one that owns g_followers, so the live map and the gambit tables are
            // legal to read here.
            std::unordered_map<RE::FormID, std::uint8_t> resolved;
            for (const auto& [id, st] : g_followers) {
                if (st.combatClassOverride >= 1 && st.combatClassOverride <= 3) {
                    resolved.emplace(id, st.combatClassOverride);
                } else if (std::find(g_activeIds.begin(), g_activeIds.end(), id) != g_activeIds.end()) {
                    if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id))
                        resolved.emplace(id, Logistics::ResolveBaseClass(a, st));
                }
            }
            std::lock_guard<std::mutex> lk(g_mx);
            g_resolvedClass.swap(resolved);
            g_tracked.clear();
            g_tracked.reserve(g_activeIds.size());
            for (const auto id : g_activeIds) g_tracked.insert(id);
            g_activeSnapshot = std::make_shared<const std::vector<RE::FormID>>(g_activeIds);
            // The per-follower MFO switch, mirrored for the progression poll on the
            // TRUE main thread (#74: never the live map). Read here on the worker —
            // the same domain that writes it (Board ApplyEdits) — so a toggle is
            // visible off-worker at once (RepublishActiveMirror at the Board write site) or by the next Refresh.
            g_mfoOff.clear();
            for (const auto& [id, st] : g_followers)
                if (!st.mfoEnabled) g_mfoOff.insert(id);
        }

        FactionQuirk g_dismissedFactions[] = {
            { "Vilja",  "EMCompViljaSkyrim.esp", 0x0D6867, -1, nullptr },
            { "Tindra", "EMCompViljaSkyrim.esp", 0x1FB779,  0, nullptr },
        };

        // Reads THE SUBJECT ACTOR's actor values.
        bool HasDismissedActorValue(RE::Actor* a_actor) {
            auto* avo = a_actor->AsActorValueOwner();
            if (!avo) return false;
            const float v = avo->GetActorValue(RE::ActorValue::kWaitingForPlayer);
            // Compare with tolerance -- it is a float AV, not an int.
            return v < (kInigoDismissedAV + 0.5f);
        }

    }

    void ResolveQuirks() {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;

        int found = 0, absent = 0;
        for (auto& q : g_dismissedFactions) {
            q.resolved = dh->LookupForm<RE::TESFaction>(q.localID, q.plugin);
            if (q.resolved) {
                ++found;
                spdlog::info("[follower] quirk active: {} ({} 0x{:06X})", q.name, q.plugin, q.localID);
            } else {
                ++absent;
                // An absent plugin is NORMAL, not an error. DYNAMIC_OR_DROP:
                // nothing is baked from this machine's load order.
                spdlog::debug("[follower] quirk inactive: {} -- {} not in load order", q.name, q.plugin);
            }
        }
        g_quirksActive   = found;
        g_quirksInactive = absent;
        spdlog::info("[follower] quirk table: {} active, {} inactive (of {})",
                     found, absent, std::size(g_dismissedFactions));
    }

    // ── ROSTER REMOVAL state (ClickUp 86e3eewaf; contract in Followers.h) ──────
    namespace {
        // Ids whose removal is in flight. WORKER domain, like g_followers (#4/#74);
        // cleared by ClearTransientState on revert/load (pump drained).
        std::unordered_set<RE::FormID> g_rosterPending;
        // The main thread's undo verdicts, handed back to the worker. g_rosterMx is a
        // strict LEAF (no MFO call is made while it is held).
        struct RosterVerdict {
            RE::FormID    id;
            bool          ok;
            std::uint64_t epoch;
        };
        std::mutex                 g_rosterMx;
        std::vector<RosterVerdict> g_rosterDone;
    }

    void ClearTransientState() {
        g_rosterPending.clear();
        { std::scoped_lock lk(g_rosterMx); g_rosterDone.clear(); }
        g_missStreak.clear();
        g_transientDrop.clear();
        g_lastRefreshAt = {}; g_transitionAt = {}; g_transitionNs.store(0, std::memory_order_relaxed); g_lastPlaceKey = nullptr; g_havePcPos = false;
        FollowerCatchUp::Reset();   // no pre-transition sample across a load
        g_lastCombat.clear();
        g_stateSample.clear();   // [follower] state-line sample (position + time)
        g_activeIds.clear();
        // Keep the off-worker mirror consistent with the just-emptied lists, or
        // IsTrackedFast/ActiveSnapshot would report stale membership across a
        // revert (g_active is cleared by ResetAllState alongside this call).
        PublishActiveMirror();
    }

    float SecondsSinceCombat(RE::FormID a_actorID) {
        const auto it = g_lastCombat.find(a_actorID);
        if (it == g_lastCombat.end()) return 1.0e9f;
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - it->second).count();
    }

    int QuirksActive()   { return g_quirksActive; }
    int QuirksInactive() { return g_quirksInactive; }

    bool IsDismissedCustomFollower(RE::Actor* a_actor) {
        if (!a_actor) return false;   // nullptr is an ERROR, never a wildcard

        if (HasDismissedActorValue(a_actor)) return true;

        for (const auto& q : g_dismissedFactions) {
            if (!q.resolved) continue;
            if (q.rank < 0) {
                if (a_actor->IsInFaction(q.resolved)) return true;
            } else {
                if (a_actor->GetFactionRank(q.resolved, false) == q.rank &&
                    a_actor->IsInFaction(q.resolved)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool IsEligibleFollower(RE::Actor* a_actor) {
        if (!a_actor) return false;

        // Cheap disqualifiers first (Swiftly Order Squad's shipped order).
        if (a_actor->IsDead())     return false;
        if (a_actor->IsDisabled()) return false;
        if (a_actor->IsDeleted())  return false;
        if (a_actor->IsPlayerRef()) return false;

        // Summons: eligible only when explicitly allowed, and NEVER persisted
        // (a runtime 0xFF FormID must not reach the co-save, INVARIANTS #9).
        if (a_actor->IsCommandedActor() && !Config::g_allowSummons) return false;

        if (!a_actor->IsPlayerTeammate()) return false;

        // ...then the quirks, which can revoke eligibility despite teammate
        // status. This ordering matters: teammate is the cheap gate, dismissal
        // is the expensive-but-authoritative one.
        if (IsDismissedCustomFollower(a_actor)) return false;

        return true;
    }

    bool IsTracked(RE::FormID a_actorID) {
        for (const auto& h : g_active) {
            if (auto* a = h.get().get(); a && a->GetFormID() == a_actorID) return true;
        }
        return false;
    }

    bool IsTrackedFast(RE::FormID a_actorID) {
        std::lock_guard<std::mutex> lk(g_mx);
        return g_tracked.find(a_actorID) != g_tracked.end();
    }

    float SecondsSinceTransition() {
        const long long ns = g_transitionNs.load(std::memory_order_relaxed);
        if (ns == 0) return -1.0f;
        return std::chrono::duration<float>(Clock::now() - Clock::time_point(Clock::duration(ns))).count();
    }

    std::shared_ptr<const std::vector<RE::FormID>> ActiveSnapshot() {
        std::lock_guard<std::mutex> lk(g_mx);
        return g_activeSnapshot;   // shared ownership; pointee is immutable
    }

    bool IsPersistableID(RE::FormID a_actorID) {
        // 0xFF is the runtime/dynamic form range. NEVER persist one
        // (INVARIANTS #9): next session the id dangles or has been REUSED by a
        // different created form, so the record silently attaches to whatever
        // now owns it.
        //
        // IsCommandedActor() alone does NOT cover this. A PlaceAtMe-spawned or
        // script-cloned teammate -- routine in a Lorerim-class order -- is not
        // "commanded" but still carries a 0xFF id. And the load side cannot
        // save us: SKSE's ResolveFormID passes 0xFF ids through as resolved.
        // So the guard has to be here, on the way in.
        return (a_actorID >> 24) != 0xFF;
    }

    // item 2b tripwire: nonzero while a MAIN-thread Board prog-edit body runs.
    // Main-thread-only counter (BoardEditScope wraps the MainThread::Post body),
    // so no lock is needed. A TryEnsureRecord INSERT observed while this is set
    // means a board edit hit a follower with no record -- the "already has a
    // record" invariant (see Followers.h BoardEditScope) was violated and we are
    // one rehash away from racing the worker's g_followers iteration.
    static int g_boardEditDepth = 0;

    BoardEditScope::BoardEditScope()  { ++g_boardEditDepth; }
    BoardEditScope::~BoardEditScope() { --g_boardEditDepth; }

    FollowerState* TryEnsureRecord(RE::FormID a_actorID) {
        if (!IsPersistableID(a_actorID)) {
            spdlog::debug("[follower] {:08X} is a runtime (0xFF) form -- session-only, no record",
                          a_actorID);
            return nullptr;
        }
        auto [it, created] = g_followers.try_emplace(a_actorID);
        if (created) {
            if (g_boardEditDepth > 0) {
                // item 2b: a main-thread board edit just INSERTED (rehash) while
                // the worker may be iterating g_followers. This is not supposed to
                // be reachable (board-addressable followers already have records);
                // shout so a future regression that breaks the invariant is caught
                // in the log instead of as an intermittent load-screen crash.
                spdlog::error("[follower] HAZARD (item 2b): main-thread board edit INSERTED a new "
                              "record for {:08X} -- g_followers rehash may race the worker. The "
                              "'board-addressable follower already has a record' invariant broke.",
                              a_actorID);
            }
            ApplyDefaultKit(it->second);   // editable base gambits, every follower
            spdlog::info("[follower] {:08X} new record -- seeded {} combat / {} logistics defaults",
                         a_actorID, it->second.combat().size(), it->second.logistics().size());
        }
        return &it->second;
    }

    FollowerState& EnsureRecord(RE::FormID a_actorID) {
        // Callers that already know the id is persistable. Kept for the sites
        // where a record must exist; the guarded form is TryEnsureRecord.
        return g_followers[a_actorID];
    }

    // a_movementOnly (claim-through-load, transient roster drop): release ONLY the holds that STEER him
    // (movement / package / target / cast claims) and KEEP the ch.17 equip claim, its change detector, the
    // Loadout ledger, the force-hold and the weapon-stance ownership. A follower looting, casting or retreating at
    // the door runs that package instead of Follow, so the engine would never bring him along; the equip
    // claim is the one thing marth said stays ("Keep the claim through loads"). The full release
    // (a_movementOnly=false) is the original, unchanged path, and is idempotent over what was kept.
    static void ReleaseHeldStateImpl(RE::FormID id, bool a_movementOnly) {
        // Hand back every bit of engine/session state MFO was holding on this
        // follower, so he reverts to a vanilla/engine-default follower. Called on
        // the WORKER: from dismissal (Refresh, above) and from the T#78 board
        // MFO-OFF toggle (Scheduler's per-follower tick, same thread). Every call
        // here is idempotent (erase-miss / no-record -> no-op), so re-running it
        // is safe. ORDER is the dismissal order, unchanged -- with ONE addition in
        // front (F7, feat/mfo-equip-authority, Fable round 2 on c66dc80): the APMF
        // EQUIP AUTHORITY is released FIRST, because the restore right below hands
        // back the follower's PRE-MFO gear (Loadout::EquipBack), which is off-set
        // under a live declaration: with the release still queued behind it, an
        // enforcing APMF would refuse the repay and the follower would leave the
        // party in MFO's gear. Logistics::OnFollowerRemoved below releases too
        // (idempotent no-op then) and drops the declaration's change detector.
        const bool authorityWasLive = !a_movementOnly && APMFBridge::IsEquipAuthorityClaimed(id);
        if (!a_movementOnly) APMFBridge::ReleaseEquipAuthority(id);
        //
        // Give back anything MFO put in their hands -- the hit sink is gated on
        // IsTracked, so after a dismissal nothing would ever restore it (#55).
        // Under the authority the restore is DEFERRED past APMF's next Drain (the
        // Release above is an enqueue, applied on APMF's per-frame game-thread
        // seat): two MainThread::Post hops land ≥1 full frame later, the same
        // shape as Actuation's PostHandEquipsDeferred, then the body rides BACK to
        // the serial AddTask worker under PumpTickGate -- Loadout's ledgers are
        // worker-serial (no lock, no cross-thread reader; Loadout.cpp:17) and must
        // not be mutated from the main thread. A revert in between makes the gate
        // bail, and the revert's ClearTransientState has cleared the ledger anyway.
        // Without the authority: the direct call, byte-identical to before.
        if (a_movementOnly) {
            // kept: Loadout ledger (restored on the full release)
        } else if (authorityWasLive) {
            const auto epoch = Diagnostics::CurrentPumpEpoch();
            auto restore = [id, epoch]() {
                SKSE::GetTaskInterface()->AddTask([id, epoch]() {
                    Diagnostics::PumpTickGate gate(epoch);
                    if (!gate) return;
                    Loadout::Restore(id);
                });
            };
            if (MainThread::IsInstalled()) MainThread::Post([restore]() { MainThread::Post(restore); });
            else                           restore();
        } else {
            Loadout::Restore(id);
        }
        // The commanded-target latch -- the hook does not check IsTracked, so a
        // latch left behind keeps redirecting the follower AND keeps every
        // Character in combat worldwide off the fast path.
        Targeting::Clear(id);
        // The engage-on-sight gambit's ch.21 entry claim + its per-follower notes
        // (Release stops no fight: Harbinger's contract). Worker, like every call here.
        EngageOnSight::Forget(id);
        // The weapon-stance ownership -- an entry left behind keeps every
        // combatant paying the ApplyTick lookup and could re-swap a style we no
        // longer own. Idempotent erase-miss when unowned.
        if (!a_movementOnly) { CombatStyle::Clear(id); CombatStyle::ClearDualGate(id); }
        // The T#76 equip force-hold: its prevent-removal LOCK is on the
        // ActorEquipManager, not the controller, so a follower left force-held
        // stays stuck with the weapon, unable to cast. Resolve the actor (the
        // handle may have merely flickered) and force-unequip; if it will not
        // resolve, the record is session-scoped and revert-cleared.
        if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && !a_movementOnly)
            Actuation::ReleaseForcedWeapon(a, /*a_standDown*/ false, /*a_leaving*/ true);   // W1: ch.17 already released above
        // The cast-consent latch -- a latch left here would DENY the follower's
        // own casting for the rest of the session and keep every combat caster
        // off the hook's fast-out.
        CasterConsent::Clear(id);
        // An out-of-combat substrate cast pair (slice S3, apmf/Substrate.cpp): Harbinger's TTL would end it within
        // seconds, but a dismissed / switched-off follower keeps no MFO claim. Idempotent (no pair -> no-op).
        APMFBridge::ReleaseSubstrateCast(id, "the follower's held state is released (dismissal / MFO off / load window)");
        // The package (cast) alias -- a longer tail: the alias fill is
        // SERIALIZED INTO THE .ess, so a latch left behind comes back on every
        // future load of every save descended from this one. Evict to the marker.
        Packages::Release(id);
        // The loot-travel alias: nothing else reclaims a walk-to-loot follower
        // and the fill is serialized, so it re-latches every load. Evict him.
        if (a_movementOnly) Logistics::OnFollowerAbsent(id);   // loot alias / slot / deposit / pick hold only
        else                Logistics::OnFollowerRemoved(id);  // + the equip claim and its change detector
        // The retreat-probe alias -- identical claim model, identical serialized
        // tail.
        Packages::RetreatEvictIf(id);
        // Harbinger ch.22 (the retreat's re-entry deny) and ch.23 (the in-combat leash):
        // runtime-only claims, released so neither outlives him. Idempotent no-ops when none.
        APMFBridge::ReleaseRetreatReentryDeny(id, "dismissed");
        APMFBridge::ReleasePursuitLeash(id, "dismissed");
        APMFBridge::ReleaseHealApproach(id, "dismissed", APMFBridge::kApproachAnyHand, true);   // ch.24 held-heal approach
        APMFBridge::ReleaseOffenseApproach(id, "dismissed", true);   // ch.24 offense approach
        // The Composed Forced Cast (OPT-IN bHealAnimPackage): runtime-only (no
        // serialized tail), but a dismissed follower's live heal-cast claim +
        // MFO-executed-cast bound must be released so neither outlives him. Both
        // calls are thread-safe (APMF mutex / CastBounds lock-free) and no-op if he
        // holds none. (Replaced the deleted Packages::HealAnimEvictIf.)
        APMFBridge::ReleaseHealCast(id);
        CastBounds::Disarm(id);
        // The offense-cast claim (ch.8b kIntent_Cast, feat/offense-cast-seats,
        // 2026-09-05): same runtime-only, no-serialized-tail shape as the heal
        // claim above -- a dismissed follower's live claim must not outlive him
        // either (Tick()'s expiry sweep would eventually drop it, but a dismissal
        // is a crisp release point, same discipline as every other claim here).
        // ClearWatch drops the shared [cfc] silent-claim diagnostic this claim
        // may have armed (Actuation::CastOn's ComposedCast::WatchClaim call).
        APMFBridge::ReleaseOffenseCast(id);
        ComposedCast::ClearWatch(id);
        // The firing-spell gambit lock (Task 2, feat/cast-gambit-concentration):
        // runtime-only, no serialized tail, same discipline as the two claims
        // just above -- a dismissed follower's lock must not outlive him either.
        Actuation::ClearCastLock(id);
    }

    void ReleaseHeldState(RE::FormID id) { ReleaseHeldStateImpl(id, false); }

    bool RequestRosterRemoval(RE::FormID id) {
        if (!IsPersistableID(id) || g_followers.find(id) == g_followers.end()) {
            spdlog::info("[roster] {:08X}: remove refused -- no MFO record", id);
            return false;
        }
        // NOT FOLLOWING = not tracked AND not an eligible follower (review F3): IsTracked
        // only sees the high process list, so a teammate told to wait or left in another
        // cell is untracked yet still a follower, and the next sweep that sees him would
        // re-adopt him with a default record. IsEligibleFollower is the teammate gate
        // minus the dismissed-custom-follower quirks (flags + actor values, no 3D / no
        // process: the read Lotd already makes on this worker for unloaded teammates).
        if (IsTracked(id) || IsEligibleFollower(RE::TESForm::LookupByID<RE::Actor>(id))) {
            // A follower in the party would be re-adopted on the next sweep with default
            // data; the T#78 MFO switch is the "keep him, leave him alone" road.
            spdlog::info("[roster] {:08X}: remove refused -- he is still a follower (in the party, waiting, "
                         "or in another cell); dismiss him first", id);
            return false;
        }
        if (g_rosterPending.count(id)) {
            spdlog::info("[roster] {:08X}: remove already in flight", id);
            return false;
        }
        if (!MainThread::IsInstalled()) {
            spdlog::error("[roster] {:08X}: remove refused -- the main-thread pump is not installed, so the "
                          "progression undo cannot run; nothing changed", id);
            return false;
        }
        spdlog::info("[roster] {:08X}: REMOVE FROM ROSTER -- releasing held state, then the main-thread undo", id);
        // Step 1: the dismissal release road, unchanged and idempotent (he was already
        // released when he left the party; this repeats it for anything since).
        ReleaseHeldState(id);
        // What NO step undoes, said loudly once (never masked): MFO keeps no record of these,
        // and each is the follower's own now (a consumed tome, a looted / bought / sold item).
        spdlog::warn("[roster] {:08X}: NOT UNDONE by design -- spells he learned from tomes (board Teach, "
                     "carried-tome auto-learn), items and gold MFO looted / bought / sold / moved for him, "
                     "and gem socket changes made through MEO stay as they are (MFO keeps no record of them)", id);
        g_rosterPending.insert(id);
        // The pump epoch moves on every revert / load (StopPump / StartPump), and it is
        // atomic, so the main closure can tell a stale post from a live one without
        // reading worker or main-only state across threads.
        const auto epoch = Diagnostics::CurrentPumpEpoch();
        MainThread::Post([id, epoch]() {
            if (Diagnostics::CurrentPumpEpoch() != epoch) {
                spdlog::info("[roster] {:08X}: removal dropped -- a revert / load intervened (every record was "
                             "reloaded from the save)", id);
                return;
            }
            bool ok = false;
            if (IsTrackedFast(id) || IsEligibleFollower(RE::TESForm::LookupByID<RE::Actor>(id))) {
                spdlog::warn("[roster] {:08X}: REMOVAL ABORTED -- he is a follower again (re-joined) before the "
                             "undo ran; every record is kept", id);
            } else {
                ok = ProgAllocator::ForgetFollower(id);   // step 2: undo + erase PRGN, main thread
            }
            std::scoped_lock lk(g_rosterMx);
            g_rosterDone.push_back({ id, ok, epoch });
        });
        return true;
    }

    void DrainRosterRemovals() {
        std::vector<RosterVerdict> done;
        {
            std::scoped_lock lk(g_rosterMx);
            if (g_rosterDone.empty()) return;
            done.swap(g_rosterDone);
        }
        const auto epoch = Diagnostics::CurrentPumpEpoch();
        for (const auto& v : done) {
            if (v.epoch != epoch) continue;   // a revert cleared everything it concerned
            g_rosterPending.erase(v.id);
            if (!v.ok) {
                spdlog::warn("[roster] {:08X}: removal not completed -- FLWR / MSTK / PGIV records kept", v.id);
                continue;
            }
            // Step 3 (worker, the domain that owns g_followers): the next save omits him
            // from every record. Progression (PRGN) was erased on the main thread already,
            // so a save landing between the two hops holds his FLWR record without PRGN,
            // a state the readers have always accepted (the records are independent).
            const bool rejoined = IsTracked(v.id);
            const auto rapport  = [&] {
                const auto it = g_followers.find(v.id);
                return it != g_followers.end() ? it->second.rapport : 0u;
            }();
            g_followers.erase(v.id);
            g_missStreak.erase(v.id);
            g_lastCombat.erase(v.id);
            g_stateSample.erase(v.id);
            Logistics::ForgetStockGear(v.id);
            PlayerGiven::Forget(v.id);
            if (rejoined) {
                // He came back between the main undo and this hop: give him what any new
                // party member gets (a fresh default record), never a recordless follower.
                TryEnsureRecord(v.id);
                spdlog::warn("[roster] {:08X}: he re-joined during the removal -- data erased, fresh default record", v.id);
            }
            PublishActiveMirror();
            spdlog::info("[roster] {:08X}: REMOVED from the roster -- FLWR (rapport {}), MSTK, PGIV and progression "
                         "erased; the next save omits him", v.id, rapport);
        }
    }

    void Refresh() {
        // The main thread's queued g_followers work, applied HERE on the record's owning
        // domain before the sweep reads the map (#4/#74): the respec rapport debits
        // (review F5), then roster removal step 3 (86e3eewaf).
        Rapport::DrainPendingSpends();
        DrainRosterRemovals();
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) {
            spdlog::warn("[follower] ProcessLists unavailable -- refresh skipped");
            return;
        }

        // World-transition marker for the claim-through-load rule (see kLoadWindowS). Four independent
        // signals, any one marks: the LoadingMenu is open; the previous sweep was > kRefreshGapS ago (the
        // worker is stalled by a load); the player's interior cell / worldspace changed; the player jumped
        // > kTeleportJumpU (fast travel). Exterior-to-exterior walking marks nothing.
        const auto sweepNow = Clock::now();
        bool       loadingNow = false;
        if (auto* ui = RE::UI::GetSingleton()) loadingNow = ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        if (loadingNow) g_transitionAt = sweepNow;
        if (g_lastRefreshAt.time_since_epoch().count() != 0 &&
            std::chrono::duration<float>(sweepNow - g_lastRefreshAt).count() > kRefreshGapS)
            g_transitionAt = sweepNow;
        g_lastRefreshAt = sweepNow;
        if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
            auto*       cell = pc->GetParentCell();
            const void* key  = (cell && cell->IsInteriorCell()) ? static_cast<const void*>(cell)
                                                                : static_cast<const void*>(pc->GetWorldspace());
            if (g_lastPlaceKey && key != g_lastPlaceKey) g_transitionAt = sweepNow;
            if (key) g_lastPlaceKey = key;
            const auto pos = pc->GetPosition();
            if (g_havePcPos && pos.GetDistance(g_lastPcPos) > kTeleportJumpU) g_transitionAt = sweepNow;
            g_lastPcPos = pos; g_havePcPos = true;
        }
        g_transitionNs.store(g_transitionAt.time_since_epoch().count(), std::memory_order_relaxed);

        // Previous set, taken from the ID list rather than by re-resolving --
        // a handle that momentarily fails to resolve must still count as a
        // MISS (and get the hold), not silently disappear (F6).
        const std::vector<RE::FormID> before = g_activeIds;

        std::vector<RE::ActorHandle> next;
        std::vector<RE::FormID>      nextIds;
        for (auto& handle : pl->highActorHandles) {
            auto* actor = handle.get().get();
            if (!actor) continue;                  // unloaded between frames
            if (!IsEligibleFollower(actor)) continue;
            next.push_back(handle);
            nextIds.push_back(actor->GetFormID());
        }

        // Deltas, by name, so the log is testable per TEST_GUIDE 2A.
        for (const auto& h : next) {
            auto* a = h.get().get();
            if (!a) continue;
            const auto id = a->GetFormID();
            g_missStreak.erase(id);   // seen -- reset any streak
            if (APMFBridge::InCombat(a)) g_lastCombat[id] = std::chrono::steady_clock::now();   // not a substrate (v26)
            if (std::find(before.begin(), before.end(), id) == before.end()) {
                const bool isSummon    = a->IsCommandedActor();
                const bool persistable = IsPersistableID(id);
                spdlog::info("[follower] + {:08X} {} ({})", id, a->GetName(),
                             isSummon      ? "summon, session-only"
                             : !persistable ? "runtime form, session-only"
                                            : "teammate");
                g_stateSample.erase(id);       // a re-join starts the moved= baseline fresh
                LogStateLine(a, id, "roster");
                // Two independent reasons to withhold a record. Checking only
                // IsCommandedActor was the gap: a cloned/spawned teammate is
                // not commanded but still has a 0xFF id.
                if (!isSummon) {
                    TryEnsureRecord(id);
                }
            } else if (const auto sit = g_stateSample.find(id);
                       sit == g_stateSample.end() ||
                       std::chrono::duration<float>(std::chrono::steady_clock::now() - sit->second.at).count() >= kStateLineS) {
                LogStateLine(a, id, "30s");
            }
        }
        const bool loading = loadingNow;
        for (const auto id : before) {
            bool still = false;
            for (const auto& h : next) {
                if (auto* a = h.get().get(); a && a->GetFormID() == id) { still = true; break; }
            }
            if (!still) {
                // Not seen this sweep -- but hold them until it repeats. DURING A LOADING
                // SCREEN the process lists are mid-rebuild and every follower reads absent,
                // so the sweep says nothing: hold him WITHOUT counting the miss, or a long
                // load drops (and ReleaseHeldState strips) a follower who never left.
                const int misses = loading ? g_missStreak[id] : ++g_missStreak[id];
                if (loading || misses < kMissesBeforeDrop) {
                    for (size_t i = 0; i < g_active.size() && i < g_activeIds.size(); ++i) {
                        if (g_activeIds[i] == id) { next.push_back(g_active[i]); nextIds.push_back(id); break; }
                    }
                    spdlog::debug("[follower] {:08X} missed sweep {}/{} -- holding",
                                  id, misses, kMissesBeforeDrop);
                    continue;
                }
                g_missStreak.erase(id);
                // WHY he is gone, said on the drop line (86e3qmmef: Sven's `-`/`+` pairs had no reason).
                auto* gone = RE::TESForm::LookupByID<RE::Actor>(id);
                const char* why = !gone                          ? "actor does not resolve"
                                : gone->IsDead()                 ? "dead"
                                : gone->IsDisabled()             ? "disabled"
                                : gone->IsDeleted()              ? "deleted"
                                : !gone->IsPlayerTeammate()      ? "no longer a teammate"
                                : IsDismissedCustomFollower(gone) ? "dismissed-custom-follower quirk"
                                : (gone->IsCommandedActor() && !Config::g_allowSummons) ? "summon, not allowed"
                                :                                  "not in the high process list (unloaded / other cell)";
                const float sinceTransition = g_transitionAt.time_since_epoch().count() != 0
                    ? std::chrono::duration<float>(sweepNow - g_transitionAt).count() : -1.0f;
                if (IsEligibleFollower(gone) && sinceTransition >= 0.0f && sinceTransition <= kLoadWindowS) {
                    // TRANSIENT (load / cell change): keep ONLY the ch.17 claim + detector, the Loadout
                    // ledger, the force-hold and the stance; release everything that steers him NOW so the
                    // engine can bring him along. The rest is released below if he stays absent or stops
                    // being eligible.
                    ReleaseHeldStateImpl(id, /*a_movementOnly*/ true);
                    g_transientDrop[id] = sweepNow;
                    spdlog::info("[follower] - {:08X} (transient, {:.1f}s after a world transition: {} -- equip claim, "
                                 "declaration and gear ledger KEPT, movement/cast/loot holds released, record and "
                                 "Rapport retained)", id, sinceTransition, why);
                    continue;
                }
                // Give back everything MFO was holding on this follower BEFORE we
                // stop tracking him (#55). The exact same release is needed when a
                // follower is toggled MFO-OFF on the board (T#78) -- one helper, one
                // source of truth (see ReleaseHeldState).
                ReleaseHeldState(id);
                // Record is RETAINED -- dismissal must never destroy Rapport
                // (DESIGN.md §3.1, the emotional core of the progression).
                spdlog::info("[follower] - {:08X} ({}; {}; record and Rapport retained)", id, why,
                             sinceTransition < 0.0f ? "no world transition seen"
                                                    : sinceTransition > kLoadWindowS ? "last world transition long ago"
                                                                                     : "not eligible");
            }
        }

        // Finish the release of transient drops that did not come back (still absent after the hold,
        // or no longer an eligible teammate). One that re-joined is simply forgotten: his claim never left.
        for (auto it = g_transientDrop.begin(); it != g_transientDrop.end();) {
            const auto id = it->first;
            if (std::find(nextIds.begin(), nextIds.end(), id) != nextIds.end()) {
                spdlog::info("[follower] {:08X} back after {:.1f}s -- his equip claim and declaration stood through the load",
                             id, std::chrono::duration<float>(sweepNow - it->second).count());
                it = g_transientDrop.erase(it);
                continue;
            }
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            const bool expired = std::chrono::duration<float>(sweepNow - it->second).count() > kTransientHoldS;
            if (expired || !IsEligibleFollower(a)) {
                ReleaseHeldState(id);
                spdlog::info("[follower] {:08X} transient drop ended -- {}; held state released", id,
                             expired ? "still absent after the hold" : "no longer an eligible teammate");
                it = g_transientDrop.erase(it);
            } else {
                ++it;
            }
        }

        g_active   = std::move(next);
        g_activeIds = std::move(nextIds);

        // Republish the off-worker mirror + snapshot from the freshly-rebuilt
        // g_activeIds. This is the ONLY safe road for the combat cast-hook /
        // sinks / SaveCallback / progression poll -- they never walk g_active.
        PublishActiveMirror();

        FollowerCatchUp::Poll();   // after a world transition, move a follower the engine left behind (INI bFollowerCatchUp)

        // Log the zero case too, or "found none" and "never ran" look the same.
        spdlog::debug("[follower] refresh: {} active, {} record(s) stored",
                      g_active.size(), g_followers.size());
    }

    // ── General follower-mutation API (v1.1 add-on-architecture SEED) ────────
    // See Followers.h for the contract + threading domain (main/serial-worker).

    // CANONICAL vital pool order — MUST match ProgAllocator's kHmsAV and the
    // PRGN v5 co-save column order {0=Health, 1=Magicka, 2=Stamina}.
    static constexpr RE::ActorValue kVitalAV[3] = {
        RE::ActorValue::kHealth, RE::ActorValue::kMagicka, RE::ActorValue::kStamina
    };

    std::uint8_t GetBaseClass(RE::FormID a_actorID) {
        const auto it = g_followers.find(a_actorID);
        return (it != g_followers.end()) ? it->second.combatClassOverride : std::uint8_t{ 0 };
    }
    std::uint8_t GetBaseClass(RE::Actor* a_actor) {
        return a_actor ? GetBaseClass(a_actor->GetFormID()) : std::uint8_t{ 0 };
    }
    bool HasEnabledEquipWeaponGambit(RE::FormID a_actorID) {
        const auto it = g_followers.find(a_actorID);
        if (it == g_followers.end()) return false;
        for (const auto& g : it->second.combat())
            if (g.enabled && (g.actionOpcode == Vocab::kActEquipMelee ||
                              g.actionOpcode == Vocab::kActEquipRanged))
                return true;
        return false;
    }
    bool IsMfoEnabled(RE::FormID a_actorID) {
        std::lock_guard<std::mutex> lk(g_mx);   // the IsTrackedFast road, never g_followers
        return g_mfoOff.find(a_actorID) == g_mfoOff.end();
    }
    std::uint8_t ResolvedClassFast(RE::FormID a_actorID) {
        std::lock_guard<std::mutex> lk(g_mx);   // the IsTrackedFast road, never g_followers
        const auto it = g_resolvedClass.find(a_actorID);
        return it != g_resolvedClass.end() ? it->second : std::uint8_t{ 0 };
    }
    // MFO-B12: public export of the anonymous-namespace PublishActiveMirror, for the Board's
    // mfoEnabled write site. WORKER only; takes g_mx itself (leaf), call under no lock.
    void RepublishActiveMirror() { PublishActiveMirror(); }

    void SetBaseClass(RE::FormID a_actorID, std::uint8_t a_stance) {
        if (auto* rec = TryEnsureRecord(a_actorID)) rec->combatClassOverride = a_stance;
    }
    void SetBaseClass(RE::Actor* a_actor, std::uint8_t a_stance) {
        if (a_actor) SetBaseClass(a_actor->GetFormID(), a_stance);
    }

    float GetFollowerHMS(RE::Actor* a_actor, int a_pool) {
        if (!a_actor || a_pool < 0 || a_pool > 2) return 0.0f;
        auto* avo = a_actor->AsActorValueOwner();
        return avo ? avo->GetBaseActorValue(kVitalAV[a_pool]) : 0.0f;
    }
    void SetFollowerHMS(RE::Actor* a_actor, int a_pool, float a_value) {
        if (!a_actor || a_pool < 0 || a_pool > 2) return;
        if (auto* avo = a_actor->AsActorValueOwner())
            avo->SetBaseActorValue(kVitalAV[a_pool], a_value);
    }

    float MeasureEngineVitalAward(RE::Actor*        a_actor,
                                  const float     (&a_heldTarget)[3],
                                  float           (&a_curOut)[3],
                                  float           (&a_deltaOut)[3]) {
        for (int p = 0; p < 3; ++p) a_curOut[p] = GetFollowerHMS(a_actor, p);
        float budget = 0.0f;
        for (int p = 0; p < 3; ++p) {
            a_deltaOut[p] = a_curOut[p] - a_heldTarget[p];
            budget       += a_deltaOut[p];
        }
        if (budget < 0.0f) budget = 0.0f;
        return budget;
    }

}
