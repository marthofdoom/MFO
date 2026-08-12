#include "PCH.h"
#include "Rapport.h"
#include "Targeting.h"
#include "Followers.h"
#include "Config.h"
#include "State.h"
#include "MainThread.h"   // #63 quash runs on the pump, never in the dispatch

namespace MFO::Rapport {

    namespace {

        // TEST_GUIDE 2D: the number BALANCE.md's entire ladder rests on and
        // which has never been measured. bProfileRapport dumps it.
        std::mutex     g_lastKillMx;
        LastKill       g_lastKill;

        std::atomic<std::uint32_t> g_sessionKills{ 0 };
        std::atomic<std::uint32_t> g_sessionRapport{ 0 };

        // What counts as a boss. LEVEL-RELATIVE ONLY.
        //
        // FIELD-CORRECTED TWICE:
        //   v0.3.0 -- IsUnique() alone missed a bandit chief with a boss bar
        //             (chiefs are leveled non-uniques).
        //   v0.4.1 -- adding IsUnique() as an OR then caught the opposite: the
        //             log showed Weylin (lvl 3) and Hogni Red-Arm (a Markarth
        //             butcher) scored as "boss x5" against a level-21 player,
        //             because both are NAMED uniques. Named is not boss.
        //
        // Both errors were the same mistake -- treating "unique" as a proxy for
        // "hard". It is not, in either direction. Level relative to the player
        // is the one signal that matches what a player means by "that was a
        // step up": a chief scaled to your level counts; a named townsperson
        // well below you does not. A true unique boss is caught when it is at
        // or above your level, which is exactly when it is actually a fight.
        //
        // Coarse by design (BALANCE.md §1.3). No IsUnique, no keyword guessing
        // (the "boss bar" has no clean bound API I have verified -- and the
        // binding-gap lesson says do not guess one).
        bool IsBoss(RE::Actor* a_victim) {
            if (!a_victim) return false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return false;
            const auto delta = static_cast<std::int32_t>(Config::g_bossLevelDelta.load());
            return static_cast<std::int32_t>(a_victim->GetLevel()) >=
                   static_cast<std::int32_t>(player->GetLevel()) + delta;
        }

        bool IsDragon(RE::Actor* a_victim) {
            if (!a_victim) return false;
            if (auto* race = a_victim->GetRace()) {
                return race->HasKeywordString("ActorTypeDragon");
            }
            return false;
        }

        // Reads THE FOLLOWER's state and the PLAYER's position. Named because
        // INVARIANTS #15 requires every scan helper to say whose state it reads.
        bool FollowerSharedTheKill(RE::Actor* a_follower, RE::Actor* a_player, RE::Actor* a_victim) {
            if (!a_follower || !a_player) return false;

            // 1. Combat state carries the archery case: the player snipes from
            //    200m while the follower fights. They are participating and
            //    distance is irrelevant. (BALANCE.md §2.)
            if (a_follower->IsInCombat()) return true;

            // 1b. RECENTLY in combat counts too, and this is the case that was
            //     silently losing awards in the field. This test runs from a
            //     QUEUED task, so by the time it asks, the victim is dead --
            //     and killing the last enemy ENDS the fight. A follower who
            //     fought the entire battle reads IsInCombat()==false at the
            //     only instant we get to look. Observed 2026-07-21: Cosnach
            //     fought a fox 2792u out and was credited nothing (#51).
            if (Followers::SecondsSinceCombat(a_follower->GetFormID()) <=
                Config::g_sharedCombatGrace.load()) {
                return true;
            }

            // 2. Radius is the fallback for stealth kills that end a fight
            //    before the follower ever engages -- measured to the PLAYER,
            //    who is the one being shared with.
            const float r = Config::g_sharedRadius.load();
            if (a_follower->GetPosition().GetDistance(a_player->GetPosition()) <= r) return true;

            // 3. Standing over the corpse is participation even when the player
            //    is far away and combat has already dropped. Without this, the
            //    follower who did the work is the one most likely to be out of
            //    the player's radius.
            if (a_victim && a_follower->GetPosition().GetDistance(a_victim->GetPosition()) <= r) {
                return true;
            }
            return false;
        }

        class DeathSink final : public RE::BSTEventSink<RE::TESDeathEvent> {
        public:
            static DeathSink* GetSingleton() {
                static DeathSink s;
                return &s;
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event,
                                                  RE::BSTEventSource<RE::TESDeathEvent>*) override {
                // TESDeathEvent FIRES TWICE. Acting on both is the classic
                // double-award (TEST_GUIDE 2B #12).
                if (!a_event || !a_event->dead) return RE::BSEventNotifyControl::kContinue;

                const auto victimHandle = a_event->actorDying
                                            ? a_event->actorDying->CreateRefHandle() : RE::ObjectRefHandle{};
                const auto killerHandle = a_event->actorKiller
                                            ? a_event->actorKiller->CreateRefHandle() : RE::ObjectRefHandle{};

                // Sinks QUEUE; they never do engine work inline (INVARIANTS #1).
                SKSE::GetTaskInterface()->AddTask([victimHandle, killerHandle]() {
                    auto* victimRef = victimHandle.get().get();
                    auto* killerRef = killerHandle.get().get();
                    auto* victim = victimRef ? victimRef->As<RE::Actor>() : nullptr;
                    auto* killer = killerRef ? killerRef->As<RE::Actor>() : nullptr;
                    if (!victim) return;

                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!player) return;

                    // GATE BEFORE THE SCAN. Without this the sink ran a full
                    // ProcessLists sweep for EVERY death in the world -- a
                    // dragon eating a giant three cells away, faction
                    // skirmishes, ambient wildlife -- and counted them all as
                    // "kills". TEST_GUIDE 2D exists to measure PARTY kills/hr
                    // against BALANCE.md's ~45/hr assumption, so counting
                    // ambient world deaths would have delivered a measurement
                    // that cannot answer the question it was built for.
                    const bool killerIsPlayer = killer && killer->IsPlayerRef();
                    const bool killerIsOurs   = killer && Followers::IsTracked(killer->GetFormID());
                    if (!killerIsPlayer && !killerIsOurs) {
                        spdlog::debug("[rapport] world death (not ours) -- ignored");
                        return;
                    }

                    float mult = 1.0f;
                    const char* kind = "standard";
                    if (IsDragon(victim))     { mult = Config::g_rapportDragonMult.load(); kind = "dragon"; }
                    else if (IsBoss(victim))  { mult = Config::g_rapportBossMult.load();   kind = "boss"; }

                    const float base = Config::g_rapportKill.load() * mult;

                    Followers::Refresh();
                    int awarded = 0;
                    // INDEX-ALIGNED with g_activeIds, because a null handle must
                    // NOT mean "no credit". Followers::Refresh deliberately holds
                    // handles that fail to resolve for up to kMissesBeforeDrop
                    // sweeps -- a 117 ms transient was already caught eating
                    // kills once. Nothing below actually needs the actor: the
                    // kill test is a FormID compare, the grace test takes a
                    // FormID, and Award takes a FormID. Skipping on `!f` would
                    // reproduce the exact field signature this fix exists to
                    // remove (#51).
                    const auto& ids = Followers::g_activeIds;
                    for (size_t i = 0; i < Followers::g_active.size(); ++i) {
                        auto* f = Followers::g_active[i].get().get();
                        const RE::FormID fid = f ? f->GetFormID()
                                                 : (i < ids.size() ? ids[i] : 0);
                        if (fid == 0) continue;                        // truly unknown
                        if (f && f->IsCommandedActor()) continue;      // summons earn nothing

                        const bool followerKilled = (killer && killer->GetFormID() == fid);
                        const bool playerKilled   = killerIsPlayer;

                        // The follower's own kills always count -- they were
                        // unambiguously fighting.
                        bool share = followerKilled;
                        if (!share && playerKilled) {
                            share = f ? FollowerSharedTheKill(f, player, victim)
                                      // No actor to measure from: fall back to the
                                      // one test that needs no position at all.
                                      : (Followers::SecondsSinceCombat(fid) <=
                                         Config::g_sharedCombatGrace.load());
                        }
                        if (!share) continue;

                        Award(fid, base, kind);
                        ++awarded;
                    }

                    // Record it for the overlay BEFORE the counters, so the
                    // classification is inspectable even when nobody qualified.
                    {
                        std::scoped_lock lk(g_lastKillMx);
                        g_lastKill.name        = victim->GetName() ? victim->GetName() : "?";
                        g_lastKill.victimLevel = victim->GetLevel();
                        g_lastKill.playerLevel = player->GetLevel();
                        g_lastKill.kind        = kind;
                        g_lastKill.awarded     = base * Config::g_rapportRate.load();
                        g_lastKill.credited    = awarded;
                        g_lastKill.valid       = true;
                    }

                    g_sessionKills.fetch_add(1);
                    // Always log the kill AND its classification, including the
                    // zero case (INVARIANTS #46). "Why was that not a boss?" and
                    // "why did nobody get credit?" must both be answerable from
                    // the log without a rebuild.
                    spdlog::info("[rapport] kill #{}: {} lvl {} (you {}) -> {} x{:.0f}, "
                                 "{} follower(s) credited",
                                 g_sessionKills.load(),
                                 victim->GetName() ? victim->GetName() : "?",
                                 victim->GetLevel(), player->GetLevel(), kind, mult, awarded);
                    if (awarded == 0) {
                        // #22j: the zero case must say why, or "no rapport" is
                        // indistinguishable from "rapport is broken".
                        if (Followers::g_active.empty()) {
                            spdlog::info("[rapport]   no followers active at kill time");
                        }
                        for (size_t i = 0; i < Followers::g_active.size(); ++i) {
                            auto* f = Followers::g_active[i].get().get();
                            if (!f) {
                                // The held-but-unresolvable case. Reporting
                                // NOTHING here is what made the field bug
                                // unreadable: an empty diagnostic block under
                                // "0 credited" says the loop never ran.
                                spdlog::info("[rapport]   no credit for {:08X}: handle unresolved "
                                             "(held by miss-streak); sinceCombat={:.1f}s",
                                             i < ids.size() ? ids[i] : 0,
                                             Followers::SecondsSinceCombat(i < ids.size() ? ids[i] : 0));
                                continue;
                            }
                            // GetName() is nullable -- and a crash HERE would be
                            // the diagnostic path taking down the thing it exists
                            // to diagnose.
                            const char* nm = f->GetName();
                            spdlog::info("[rapport]   no credit for {:08X} {}: inCombat={} "
                                         "sinceCombat={:.1f}s distToPlayer={:.0f}u distToVictim={:.0f}u",
                                         f->GetFormID(), nm ? nm : "?", f->IsInCombat() ? "Y" : "n",
                                         Followers::SecondsSinceCombat(f->GetFormID()),
                                         f->GetPosition().GetDistance(player->GetPosition()),
                                         f->GetPosition().GetDistance(victim->GetPosition()));
                        }
                    }
                });

                return RE::BSEventNotifyControl::kContinue;
            }
        };

        std::atomic<std::uint32_t> g_combatEvents{ 0 };
        std::chrono::steady_clock::time_point g_sessionStart = std::chrono::steady_clock::now();

        class CombatSink final : public RE::BSTEventSink<RE::TESCombatEvent> {
        public:
            static CombatSink* GetSingleton() {
                static CombatSink s;
                return &s;
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* a_event,
                                                  RE::BSTEventSource<RE::TESCombatEvent>*) override {
                if (!a_event || !a_event->actor) return RE::BSEventNotifyControl::kContinue;

                // TESCombatEvent is a GLOBAL source -- it fires for every actor
                // in the load order. MRO's lesson was that the COST IS THE
                // DISPATCH, not the handler body. So the very first thing this
                // does is a cheap teammate test, before any allocation, any
                // handle creation, any task queue. TEST_GUIDE 2C measures
                // whether even this is enough.
                auto* actor = a_event->actor->As<RE::Actor>();
                if (!actor || !actor->IsPlayerTeammate()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                ++g_combatEvents;

                // #63 QUASH ALLY COMBAT. Followers must NEVER fight each other.
                // When a teammate ENTERS/updates combat (newState != kNone) against
                // a target that is ALSO a player teammate -- friendly fire from a
                // forced offensive cast is the usual inducer, and the friendly-fire
                // hold is inert on this runtime -- end that fight on BOTH sides so
                // neither keeps the other latched. NEVER INLINE (v1.0.53 deck
                // freeze): StopCombat itself re-fires combat events, so calling
                // it from INSIDE this dispatch re-enters the event source that
                // is mid-dispatch -- while the Scheduler backstop was firing the
                // same pair of StopCombats concurrently from the worker tick.
                // Re-entrant event lock + cross-thread combat mutation on the
                // same actors = 94 threads parked, hard freeze; the old "no
                // loop" claim here was simply wrong. QuashAllyPair DEFERS the
                // work out of this dispatch (main-thread pump; SKSE task queue
                // on VR) and holds a shared per-pair ~2 s cooldown, so the
                // every-180ms re-quash churn cannot come back either. The
                // Scheduler tick backstops the case where BOTH were already in
                // combat (no enter-event fires for the crossfire).
                if (Config::g_quashAllyCombat.load() &&
                    a_event->newState != RE::ACTOR_COMBAT_STATE::kNone &&
                    a_event->targetActor) {
                    if (auto* tgt = a_event->targetActor->As<RE::Actor>();
                        tgt && tgt != actor && tgt->IsPlayerTeammate()) {
                        if (QuashAllyPair(actor->GetFormID(), tgt->GetFormID())) {
                            spdlog::info("[peace] {:08X} entered combat vs teammate {:08X} -- quashed (StopCombat both)",
                                         actor->GetFormID(), tgt->GetFormID());
                        }
                    }
                }

                // COMBAT ENDED -> RELEASE THE LATCH.
                //
                // A commanded target that outlives its fight is not just stale,
                // it is WRONG: in the follower's next fight the hook sees a live
                // `wanted` and a live current target that differ, and steers
                // them at last fight's foe. #59's guard does not cover "that foe
                // is not in THIS fight".
                //
                // It also silently defeats the hook's fast path -- g_latchCount
                // never returns to zero, so every Character in combat in the
                // world pays the lock-and-lookup for the rest of the session.
                if (a_event->newState == RE::ACTOR_COMBAT_STATE::kNone) {
                    const auto id = actor->GetFormID();
                    SKSE::GetTaskInterface()->AddTask([id]() { Targeting::Clear(id); });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

    }

    // ── #63 QUASH -- the thread-safe half (v1.0.53 deck freeze) ──────────────
    namespace {
        // Per-PAIR cooldown ledger. A streamed concentration cast re-aggroed
        // the same pair every frame, so the quash re-fired every ~180 ms from
        // two threads at once; the cooldown makes one quash per pair per
        // window, whichever site (event sink or tick backstop) gets there
        // first. LEAF mutex (Sightline's rule): nothing is called while it is
        // held, and the Post below happens strictly OUTSIDE it.
        std::mutex g_quashMx;
        std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> g_quashNext;
        constexpr auto kQuashPairCooldown = std::chrono::seconds(2);
    }

    bool QuashAllyPair(RE::FormID a_actor, RE::FormID a_target) {
        // Canonical key -- (A,B) and (B,A) share one cooldown entry.
        const auto lo  = a_actor < a_target ? a_actor : a_target;
        const auto hi  = a_actor < a_target ? a_target : a_actor;
        const auto key = (static_cast<std::uint64_t>(lo) << 32) | hi;
        {
            std::lock_guard lk(g_quashMx);
            const auto now = std::chrono::steady_clock::now();
            // Opportunistic prune: an expired window is dead weight, and the
            // map otherwise grows by one entry per unique pair per session.
            // (This may drop THIS pair's expired entry too; the lookup below
            // then recreates it at epoch, which reads as "never quashed" --
            // the same verdict its expired window would have produced.)
            std::erase_if(g_quashNext, [&](const auto& e) { return now >= e.second; });
            auto& next = g_quashNext[key];
            if (next.time_since_epoch().count() != 0 && now < next) return false;
            next = now + kQuashPairCooldown;
        }

        // The WORK runs on a DEFERRED stack -- the main thread next frame
        // (non-VR), or the SKSE task queue's worker (VR) -- never inside the
        // TESCombatEvent dispatch (StopCombat re-fires combat events).
        // FormIDs, never pointers: whatever runs this later re-resolves
        // against the live world (Sightline's rule).
        auto work = [a_actor, a_target]() {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(a_actor);
            auto* b = RE::TESForm::LookupByID<RE::Actor>(a_target);
            if (a) a->StopCombat();
            if (b) b->StopCombat();
            // Best-effort de-aggro: if either still holds the OTHER as combat
            // target, drop it (the same fields Targeting's thunk writes --
            // both below the §0.29 AE layout divergence), so the next combat
            // update does not instantly re-acquire the ally.
            auto drop = [](RE::Actor* a_x, RE::Actor* a_other) {
                if (!a_x || !a_other) return;
                auto& rt = a_x->GetActorRuntimeData();
                auto curPtr = rt.currentCombatTarget.get();   // HOLD the NiPointer
                if (curPtr.get() != a_other) return;
                rt.currentCombatTarget = RE::ActorHandle{};
                if (auto* cc = rt.combatController) cc->targetHandle = RE::ActorHandle{};
            };
            drop(a, b);
            drop(b, a);
        };
        // Defer OUT of the caller's stack on EVERY runtime -- an inline call
        // from the event sink would still run StopCombat inside the combat-
        // event dispatch, which is the re-entrancy half of the freeze; the
        // pair cooldown caps CHURN, it does nothing for re-entrancy. Non-VR:
        // the main-thread pump (branch on VR, not IsInstalled() -- pre-
        // Install Post queues safely and drains once live, while
        // IsInstalled() is false then). VR: the pump is a documented no-op,
        // so defer via the SKSE task queue instead -- a WORKER, not main
        // ([[skse-addtask-runs-on-job-worker]]), but a deferred stack all the
        // same: the sink returns before any StopCombat runs. The cooldown
        // also means only ONE site fires per window, so VR gets a single
        // deferred worker-side StopCombat -- the same exposure class as the
        // long-shipped auto-retreat path.
        if (REL::Module::IsVR()) SKSE::GetTaskInterface()->AddTask(work);
        else                     MainThread::Post(std::move(work));
        return true;
    }

    std::uint32_t CombatEventCount() { return g_combatEvents.load(); }
    std::uint32_t SessionKills()     { return g_sessionKills.load(); }
    std::uint32_t SessionRapport()   { return g_sessionRapport.load(); }

    LastKill GetLastKill() {
        std::scoped_lock lk(g_lastKillMx);
        return g_lastKill;
    }

    void ResetSessionCounters() {
        // Save-scoped: without this the kills/hr denominator includes main-menu
        // time and every previously-loaded save in the same process.
        g_sessionKills   = 0;
        g_sessionRapport = 0;
        g_combatEvents   = 0;
        g_sessionStart   = std::chrono::steady_clock::now();
        { std::scoped_lock lk(g_lastKillMx); g_lastKill = {}; }
        // The quash cooldown ledger is transient too: steady_clock survives a
        // load, so a stale entry from the LAST save would silently swallow the
        // next session's first quash of that pair for up to the window.
        { std::scoped_lock lk(g_quashMx); g_quashNext.clear(); }
    }

    double SessionMinutes() {
        using namespace std::chrono;
        return duration_cast<seconds>(steady_clock::now() - g_sessionStart).count() / 60.0;
    }

    std::uint8_t RankFor(std::uint32_t a_rapport) {
        if (a_rapport >= static_cast<std::uint32_t>(Config::g_rank5.load())) return 5;
        if (a_rapport >= static_cast<std::uint32_t>(Config::g_rank4.load())) return 4;
        if (a_rapport >= static_cast<std::uint32_t>(Config::g_rank3.load())) return 3;
        if (a_rapport >= static_cast<std::uint32_t>(Config::g_rank2.load())) return 2;
        return 1;
    }

    void Award(RE::FormID a_actorID, float a_amount, const char* a_reason) {
        const float scaled = a_amount * Config::g_rapportRate.load();
        if (scaled <= 0.0f) return;

        // TryEnsureRecord, NOT EnsureRecord. The award loop skips commanded
        // summons, but g_active legitimately holds PlaceAtMe/cloned teammates
        // with 0xFF ids -- exactly the case IsCommandedActor does not cover.
        // EnsureRecord would create a doomed record that SaveCallback then has
        // to skip, leaving the invariant standing only on its last line of
        // defence (F2).
        auto* rec = Followers::TryEnsureRecord(a_actorID);
        if (!rec) return;
        auto& st = *rec;
        const auto before = st.rank;
        st.rapport += static_cast<std::uint32_t>(scaled + 0.5f);
        st.rank = RankFor(st.rapport);
        g_sessionRapport.fetch_add(static_cast<std::uint32_t>(scaled + 0.5f));

        // Log EVERY award at info. Previously this only spoke on a rank change
        // or under bProfileRapport, so the log had nothing to say about the one
        // thing being tested (found by reading a real session log, 2026-07-21).
        spdlog::info("[rapport] {:08X} +{:.1f} ({}) -> {}", a_actorID, scaled, a_reason, st.rapport);

        if (st.rank != before) {
            spdlog::info("[rapport] {:08X} RANK {} -> {} at {} rapport "
                         "(combat slots {} -> {}, logistics {} -> {})",
                         a_actorID, before, st.rank, st.rapport,
                         SlotsForRank(before, Table::Combat),    SlotsForRank(st.rank, Table::Combat),
                         SlotsForRank(before, Table::Logistics), SlotsForRank(st.rank, Table::Logistics));

            // FLAIR #13: surface the rank TRANSITION on the HUD -- the
            // fighting-together system's "the party grows" beat. Transition-
            // only by construction (rank changes are rare by BALANCE design),
            // gated behind a default-on toggle for anyone who wants a clean
            // HUD. DebugNotification just enqueues a UI message, the same call
            // other queued-task paths ship; copy stays short and dry.
            if (Config::g_rapportToasts.load()) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(a_actorID);
                const char* who = (a && a->GetName() && a->GetName()[0])
                                      ? a->GetName() : "Your follower";
                const std::string msg =
                    std::format("{} fights beside you with new resolve.", who);
                RE::DebugNotification(msg.c_str());
            }
        }
    }

    void Spend(RE::FormID a_actorID, float a_amount, const char* a_reason) {
        // The DEBIT twin of Award, added for the progression respec (§15:
        // respec is free of gold but costs rapport — the follower resents the
        // reset). Deliberately NOT rate-scaled: a cost is a price tag, not an
        // accrual, and fRapportRate must not discount it.
        if (a_amount <= 0.0f) return;
        auto* rec = Followers::TryEnsureRecord(a_actorID);
        if (!rec) return;
        auto& st = *rec;
        const auto before = st.rank;
        const auto cut = static_cast<std::uint32_t>(a_amount + 0.5f);
        st.rapport = (st.rapport > cut) ? st.rapport - cut : 0u;
        st.rank = RankFor(st.rapport);
        spdlog::info("[rapport] {:08X} -{} ({}) -> {}", a_actorID, cut, a_reason, st.rapport);
        if (st.rank != before) {
            // A DOWN transition can shrink the slot ladder below rules already
            // authored; nothing is deleted here — the load-time clamp is the
            // enforcement point — but say it plainly.
            spdlog::info("[rapport] {:08X} RANK {} -> {} at {} rapport (spend: '{}'; slots may now "
                         "exceed the ladder until the next load clamps them)",
                         a_actorID, before, st.rank, st.rapport, a_reason);
        }
    }

    void RegisterSinks() {
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            spdlog::error("[rapport] ScriptEventSourceHolder unavailable -- NO SINKS REGISTERED");
            return;
        }
        holder->AddEventSink<RE::TESDeathEvent>(DeathSink::GetSingleton());
        holder->AddEventSink<RE::TESCombatEvent>(CombatSink::GetSingleton());
        spdlog::info("[rapport] sinks registered (death, combat)");
    }

}
