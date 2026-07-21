#include "PCH.h"
#include "Rapport.h"
#include "Followers.h"
#include "Config.h"
#include "State.h"

namespace MFO::Rapport {

    namespace {

        // TEST_GUIDE 2D: the number BALANCE.md's entire ladder rests on and
        // which has never been measured. bProfileRapport dumps it.
        std::atomic<std::uint32_t> g_sessionKills{ 0 };
        std::atomic<std::uint32_t> g_sessionRapport{ 0 };

        bool IsBoss(RE::Actor* a_victim) {
            if (!a_victim) return false;
            // Coarse by design (BALANCE.md §1.3): no per-enemy difficulty
            // scaling, because it is unknowable per install and would make the
            // ladder unpredictable. Unique actors count as named/boss.
            if (auto* base = a_victim->GetActorBase()) {
                if (base->IsUnique()) return true;
            }
            return false;
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
        bool FollowerSharedTheKill(RE::Actor* a_follower, RE::Actor* a_player) {
            if (!a_follower || !a_player) return false;

            // 1. Combat state carries the archery case: the player snipes from
            //    200m while the follower fights. They are participating and
            //    distance is irrelevant. (BALANCE.md §2.)
            if (a_follower->IsInCombat()) return true;

            // 2. Radius is the fallback for stealth kills that end a fight
            //    before the follower ever engages.
            const float r = Config::g_sharedRadius.load();
            return a_follower->GetPosition().GetDistance(a_player->GetPosition()) <= r;
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

                    float mult = 1.0f;
                    const char* kind = "standard";
                    if (IsDragon(victim))     { mult = Config::g_rapportDragonMult.load(); kind = "dragon"; }
                    else if (IsBoss(victim))  { mult = Config::g_rapportBossMult.load();   kind = "boss"; }

                    const float base = Config::g_rapportKill.load() * mult;

                    Followers::Refresh();
                    int awarded = 0;
                    for (const auto& h : Followers::g_active) {
                        auto* f = h.get().get();
                        if (!f) continue;
                        if (f->IsCommandedActor()) continue;   // summons earn nothing

                        const bool followerKilled = (killer && killer->GetFormID() == f->GetFormID());
                        const bool playerKilled   = (killer && killer->IsPlayerRef());

                        // The follower's own kills always count -- they were
                        // unambiguously fighting.
                        bool share = followerKilled;
                        if (!share && playerKilled) {
                            share = FollowerSharedTheKill(f, player);
                        }
                        if (!share) continue;

                        Award(f->GetFormID(), base, kind);
                        ++awarded;
                    }

                    g_sessionKills.fetch_add(1);
                    if (Config::g_profileRapport.load()) {
                        spdlog::info("[rapport-profile] kill #{} ({}), {} follower(s) credited, "
                                     "session rapport {}",
                                     g_sessionKills.load(), kind, awarded, g_sessionRapport.load());
                    } else if (awarded == 0) {
                        // Log the zero case (INVARIANTS #46): "nobody qualified"
                        // and "the sink never ran" must not look identical.
                        spdlog::debug("[rapport] kill ({}) -- no follower qualified", kind);
                    }
                });

                return RE::BSEventNotifyControl::kContinue;
            }
        };

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
                return RE::BSEventNotifyControl::kContinue;
            }

            static inline std::atomic<std::uint32_t> g_combatEvents{ 0 };
        };

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

        auto& st = Followers::EnsureRecord(a_actorID);
        const auto before = st.rank;
        st.rapport += static_cast<std::uint32_t>(scaled + 0.5f);
        st.rank = RankFor(st.rapport);
        g_sessionRapport.fetch_add(static_cast<std::uint32_t>(scaled + 0.5f));

        if (st.rank != before) {
            spdlog::info("[rapport] {:08X} RANK {} -> {} at {} rapport "
                         "(combat slots {} -> {}, logistics {} -> {})",
                         a_actorID, before, st.rank, st.rapport,
                         SlotsForRank(before, Table::Combat),    SlotsForRank(st.rank, Table::Combat),
                         SlotsForRank(before, Table::Logistics), SlotsForRank(st.rank, Table::Logistics));
        } else if (Config::g_profileRapport.load()) {
            spdlog::info("[rapport] {:08X} +{:.1f} ({}) -> {}", a_actorID, scaled, a_reason, st.rapport);
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
