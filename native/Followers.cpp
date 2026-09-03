#include "PCH.h"
#include "Followers.h"
#include "Loadout.h"
#include "Targeting.h"
#include "CasterConsent.h"   // v1.0.30: dismissal releases the cast latch too
#include "CombatStyle.h"     // v1.0.33: dismissal drops weapon-stance ownership
#include "Actuation.h"       // #76: dismissal releases the equip force-hold too
#include "Packages.h"
#include "Logistics.h"
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
        std::shared_ptr<const std::vector<RE::FormID>>   g_activeSnapshot =
            std::make_shared<const std::vector<RE::FormID>>();

        // Republish the mirror + snapshot from the current g_activeIds. Call
        // under NOTHING (it takes g_mx itself); invoked at Refresh's tail and
        // from ClearTransientState. g_mx is a strict LEAF -- no MFO call is made
        // while it is held.
        void PublishActiveMirror() {
            std::lock_guard<std::mutex> lk(g_mx);
            g_tracked.clear();
            g_tracked.reserve(g_activeIds.size());
            for (const auto id : g_activeIds) g_tracked.insert(id);
            g_activeSnapshot = std::make_shared<const std::vector<RE::FormID>>(g_activeIds);
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

    void ClearTransientState() {
        g_missStreak.clear();
        g_lastCombat.clear();
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

    void ReleaseHeldState(RE::FormID id) {
        // Hand back every bit of engine/session state MFO was holding on this
        // follower, so he reverts to a vanilla/engine-default follower. Called on
        // the WORKER: from dismissal (Refresh, above) and from the #78 board
        // MFO-OFF toggle (Scheduler's per-follower tick, same thread). Every call
        // here is idempotent (erase-miss / no-record -> no-op), so re-running it
        // is safe. ORDER is the dismissal order, unchanged.
        //
        // Give back anything MFO put in their hands -- the hit sink is gated on
        // IsTracked, so after a dismissal nothing would ever restore it (#55).
        Loadout::Restore(id);
        // The commanded-target latch -- the hook does not check IsTracked, so a
        // latch left behind keeps redirecting the follower AND keeps every
        // Character in combat worldwide off the fast path.
        Targeting::Clear(id);
        // The weapon-stance ownership -- an entry left behind keeps every
        // combatant paying the ApplyTick lookup and could re-swap a style we no
        // longer own. Idempotent erase-miss when unowned.
        CombatStyle::Clear(id);
        // The #76 equip force-hold: its prevent-removal LOCK is on the
        // ActorEquipManager, not the controller, so a follower left force-held
        // stays stuck with the weapon, unable to cast. Resolve the actor (the
        // handle may have merely flickered) and force-unequip; if it will not
        // resolve, the record is session-scoped and revert-cleared.
        if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id))
            Actuation::ReleaseForcedWeapon(a);
        // The cast-consent latch -- a latch left here would DENY the follower's
        // own casting for the rest of the session and keep every combat caster
        // off the hook's fast-out.
        CasterConsent::Clear(id);
        // The package (cast) alias -- a longer tail: the alias fill is
        // SERIALIZED INTO THE .ess, so a latch left behind comes back on every
        // future load of every save descended from this one. Evict to the marker.
        Packages::Release(id);
        // The loot-travel alias: nothing else reclaims a walk-to-loot follower
        // and the fill is serialized, so it re-latches every load. Evict him.
        Logistics::OnFollowerRemoved(id);
        // The retreat-probe alias -- identical claim model, identical serialized
        // tail.
        Packages::RetreatEvictIf(id);
    }

    void Refresh() {
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) {
            spdlog::warn("[follower] ProcessLists unavailable -- refresh skipped");
            return;
        }

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
            if (a->IsInCombat()) g_lastCombat[id] = std::chrono::steady_clock::now();
            if (std::find(before.begin(), before.end(), id) == before.end()) {
                const bool isSummon    = a->IsCommandedActor();
                const bool persistable = IsPersistableID(id);
                spdlog::info("[follower] + {:08X} {} ({})", id, a->GetName(),
                             isSummon      ? "summon, session-only"
                             : !persistable ? "runtime form, session-only"
                                            : "teammate");
                // Two independent reasons to withhold a record. Checking only
                // IsCommandedActor was the gap: a cloned/spawned teammate is
                // not commanded but still has a 0xFF id.
                if (!isSummon) {
                    TryEnsureRecord(id);
                }
            }
        }
        for (const auto id : before) {
            bool still = false;
            for (const auto& h : next) {
                if (auto* a = h.get().get(); a && a->GetFormID() == id) { still = true; break; }
            }
            if (!still) {
                // Not seen this sweep -- but hold them until it repeats.
                const int misses = ++g_missStreak[id];
                if (misses < kMissesBeforeDrop) {
                    for (size_t i = 0; i < g_active.size() && i < g_activeIds.size(); ++i) {
                        if (g_activeIds[i] == id) { next.push_back(g_active[i]); nextIds.push_back(id); break; }
                    }
                    spdlog::debug("[follower] {:08X} missed sweep {}/{} -- holding",
                                  id, misses, kMissesBeforeDrop);
                    continue;
                }
                g_missStreak.erase(id);
                // Give back everything MFO was holding on this follower BEFORE we
                // stop tracking him (#55). The exact same release is needed when a
                // follower is toggled MFO-OFF on the board (#78) -- one helper, one
                // source of truth (see ReleaseHeldState).
                ReleaseHeldState(id);
                // Record is RETAINED -- dismissal must never destroy Rapport
                // (DESIGN.md §3.1, the emotional core of the progression).
                spdlog::info("[follower] - {:08X} (record and Rapport retained)", id);
            }
        }

        g_active   = std::move(next);
        g_activeIds = std::move(nextIds);

        // Republish the off-worker mirror + snapshot from the freshly-rebuilt
        // g_activeIds. This is the ONLY safe road for the combat cast-hook /
        // sinks / SaveCallback / progression poll -- they never walk g_active.
        PublishActiveMirror();

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
