#include "PCH.h"
#include "Diagnostics.h"
#include "Followers.h"
#include "Rapport.h"
#include "Config.h"
#include "Forms.h"
#include "State.h"
#include "Board.h"
#include "Probe.h"
#include "Scheduler.h"
#include "Loadout.h"
#include "Papyrus.h"
#include "Targeting.h"

// The M3 test instrument.
//
// There is no UI until M7, so everything TEST_GUIDE session 2 asks for has to
// be readable out of MFO.log. Two mechanisms:
//
//   1. A periodic refresh, so recruiting/dismissing a follower SHOWS UP.
//      Without it detection only updated on load or on a kill, which made
//      most of matrix 2A unobservable -- the tests would have "passed" by
//      producing no output at all.
//   2. The Field Orders power dumps a full state report on cast. The power is
//      granted from M2 and does nothing until M7, so this costs nothing and
//      doubles as proof that the TESSpellCastEvent sink works -- which is
//      exactly what M7's board opener needs.

namespace MFO::Diagnostics {

    namespace {

        // THE EVALUATOR OWNS THIS NUMBER NOW (§4.1). It used to be a HUD
        // refresh rate, and when M5 started riding it the "133 ms" tick was
        // silently whatever the HUD happened to want -- a review caught the
        // evaluator running 4x slow because a display constant moved. The pump
        // wakes at the response deadline; DIAGNOSTICS subsample it instead.
        constexpr std::uint32_t kPumpMs        = 133;
        constexpr std::uint32_t kDiagEveryNth  = 4;    // ~532 ms, the old cadence
        std::atomic<bool>          g_pumpRunning{ false };
        // Generation token: StopPump/StartPump bump it, so a thread that was
        // mid-sleep across a revert->load exits instead of running alongside
        // its own replacement.
        std::atomic<std::uint64_t> g_pumpEpoch{ 0 };

        // The shield restore trigger (DESIGN §4.5b). A shield only matters when
        // something is hitting you, so that is exactly when MFO gives it back
        // -- instead of churning equip/unequip after every cast.
        class HitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
        public:
            static HitSink* GetSingleton() { static HitSink s; return &s; }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event,
                                                  RE::BSTEventSource<RE::TESHitEvent>*) override {
                if (!a_event || !a_event->target) return RE::BSEventNotifyControl::kContinue;
                auto* actor = a_event->target->As<RE::Actor>();
                if (!actor) return RE::BSEventNotifyControl::kContinue;
                const auto id = actor->GetFormID();
                if (!Followers::IsTracked(id)) return RE::BSEventNotifyControl::kContinue;

                // Sinks QUEUE; equipping is engine work (INVARIANTS #1).
                SKSE::GetTaskInterface()->AddTask([id]() { Loadout::OnFollowerHit(id); });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        class SpellSink final : public RE::BSTEventSink<RE::TESSpellCastEvent> {
        public:
            static SpellSink* GetSingleton() {
                static SpellSink s;
                return &s;
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
                                                  RE::BSTEventSource<RE::TESSpellCastEvent>*) override {
                if (!a_event || !a_event->object) return RE::BSEventNotifyControl::kContinue;
                if (!a_event->object->IsPlayerRef())  return RE::BSEventNotifyControl::kContinue;
                if (!Forms::g_fieldOrders)            return RE::BSEventNotifyControl::kContinue;
                if (a_event->spell != Forms::g_fieldOrders->GetFormID())
                    return RE::BSEventNotifyControl::kContinue;

                // Sinks queue; they never do engine work inline.
                SKSE::GetTaskInterface()->AddTask([]() {
                    if (Board::IsAvailable()) {
                        Board::PublishSnapshot();
                        Board::Toggle();
                    } else {
                        // The overlay is not the only way to see state, and the
                        // evaluator must never depend on the renderer
                        // (INVARIANTS #25). Fall back to the log.
                        DumpReport("power (overlay unavailable)");
                    }
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // ONE persistent sleeper thread that only ever QUEUES to main -- the
        // family's established pattern ("sinks, sleeper threads, and menu
        // actions only queue"). Spawning a detached thread per tick would be
        // a thread-per-2s leak and a lifetime hazard on shutdown.
        //
        // This is the M3 stand-in for M5's real scheduler: deliberately dumb
        // and slow, because detection changes are all it needs to catch.
        void SleeperLoop(std::uint64_t a_epoch) {
            std::uint32_t wake = 0;
            while (g_pumpRunning.load() && g_pumpEpoch.load() == a_epoch) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kPumpMs));
                if (!g_pumpRunning.load() || g_pumpEpoch.load() != a_epoch) break;

                const bool diagTurn = (++wake % kDiagEveryNth) == 0;

                // Null-check: on shutdown the interface can be gone while
                // this thread is still awake.
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([a_epoch, diagTurn]() {
                        if (!g_pumpRunning.load() || g_pumpEpoch.load() != a_epoch) return;

                        // Detection and the HUD stay on the old ~532 ms budget;
                        // only the evaluator runs at the deadline.
                        if (diagTurn) Followers::Refresh();

                        Scheduler::Tick();
                        Loadout::Tick();   // hand back stowed two-handers

                        if (diagTurn) {
                            Probe::Tick();
                            Board::PublishSnapshot();
                        }
                    });
                }
            }
        }

    }

    void DumpReport(const char* a_trigger) {
        const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
        const auto  v      = plugin->GetVersion();

        spdlog::info("================ MFO STATE REPORT ({}) ================", a_trigger);
        spdlog::info("  MFO {}.{}.{}  |  game {}", v.major(), v.minor(), v.patch(),
                     REL::Module::get().version().string());

        // -- config actually in force (not what the INI says on disk) --------
        spdlog::info("  config: rate={:.2f} kill={:.2f} boss={:.1f}x dragon={:.1f}x "
                     "radius={:.0f} summons={} ranks={}/{}/{}/{}",
                     Config::g_rapportRate.load(), Config::g_rapportKill.load(),
                     Config::g_rapportBossMult.load(), Config::g_rapportDragonMult.load(),
                     Config::g_sharedRadius.load(), Config::g_allowSummons.load() ? "on" : "off",
                     Config::g_rank2.load(), Config::g_rank3.load(),
                     Config::g_rank4.load(), Config::g_rank5.load());

        // -- TEST_GUIDE 2C: the dispatch-volume question -----------------
        const double mins = Rapport::SessionMinutes();
        const auto   ce   = Rapport::CombatEventCount();
        // EVALUATOR HEARTBEAT. Without this, "no rule fired" and "the evaluator
        // never ran" produce an IDENTICAL log -- which is exactly what the
        // 0.5.1 session hit: seeds landed, no [eval] line appeared, and nothing
        // could distinguish correct silence from a dead loop. Tick counters
        // existed since 0.5.0 and were surfaced nowhere (#53).
        spdlog::info("  evaluator: {} tick(s) this session, last {:.3f} ms  [{}]",
                     Scheduler::TicksThisSession(), Scheduler::LastTickMs(),
                     Scheduler::TicksThisSession() == 0
                         ? "NEVER RAN -- pump or gating problem"
                         : "running");
        spdlog::info("  loadout: {} follower(s) owed displaced gear", Loadout::PendingRestores());
        {
            const auto t = Targeting::GetStats();
            spdlog::info("  targeting: hook {} | {} latched | {} assert(s), {} drift(s), {} pass(es){}",
                         Targeting::IsHooked() ? "INSTALLED" : "off",
                         t.latched, t.asserts, t.drifts, t.passes,
                         t.conflictMod ? "  [SmartNPCTargetSelector ALSO LOADED]" : "");
        }
        spdlog::info("  papyrus VM: {} (dispatched {}, failed {})",
                     Papyrus::Available() ? "reachable" : "UNREACHABLE -- commanded casts disabled",
                     Papyrus::Dispatches(), Papyrus::Failures());
        spdlog::info("  combat events (teammate-filtered): {} in {:.1f} min ({:.1f}/min)",
                     ce, mins, mins > 0.01 ? ce / mins : 0.0);

        // -- TEST_GUIDE 2D: the number BALANCE.md rests on -------------------
        const auto kills = Rapport::SessionKills();
        const auto rap   = Rapport::SessionRapport();
        spdlog::info("  session: {} kill(s), {} rapport, {:.1f} min "
                     "=> {:.1f} kills/hr, {:.1f} rapport/hr   <-- BALANCE.md 1.1 assumes ~45/hr",
                     kills, rap, mins,
                     mins > 0.01 ? kills * 60.0 / mins : 0.0,
                     mins > 0.01 ? rap   * 60.0 / mins : 0.0);

        // -- active followers ------------------------------------------------
        spdlog::info("  ACTIVE followers: {}", Followers::g_active.size());
        for (const auto& h : Followers::g_active) {
            auto* a = h.get().get();
            if (!a) { spdlog::info("    <handle no longer resolves>"); continue; }
            const auto id = a->GetFormID();
            // find(), NEVER operator[]. operator[] INSERTS, so one cast of
            // this diagnostic would create the record Refresh deliberately
            // withheld from a summon -- keyed on a 0xFF runtime FormID that
            // SaveCallback would then persist (INVARIANTS #9). A diagnostic
            // must never mutate authoritative state.
            const auto it = g_followers.find(id);
            if (it == g_followers.end()) {
                spdlog::info("    {:08X} {:<20} (no record -- session-only)", id, a->GetName());
                continue;
            }
            const auto& st = it->second;
            spdlog::info("    {:08X} {:<20} rapport {:>6}  rank {}  slots {}c/{}l  "
                         "[teammate={} commanded={} inCombat={}]",
                         id, a->GetName(), st.rapport, st.rank,
                         SlotsForRank(st.rank, Table::Combat),
                         SlotsForRank(st.rank, Table::Logistics),
                         a->IsPlayerTeammate() ? "Y" : "n",
                         a->IsCommandedActor() ? "Y" : "n",
                         a->IsInCombat()       ? "Y" : "n");
        }

        // -- every stored record, including dismissed ------------------------
        // Proves TEST_GUIDE 2A #4/#5: dismissal must RETAIN the record.
        spdlog::info("  STORED records: {} (dismissed followers keep theirs)", g_followers.size());
        for (const auto& [id, st] : g_followers) {
            const bool active = Followers::IsTracked(id);
            spdlog::info("    {:08X} rapport {:>6}  rank {}  gambits {}c/{}l  {}",
                         id, st.rapport, st.rank,
                         st.combat().size(), st.logistics().size(),
                         active ? "ACTIVE" : "(inactive - retained)");
        }
        spdlog::info("======================================================");
    }

    void Install() {
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (holder) {
            holder->AddEventSink<RE::TESSpellCastEvent>(SpellSink::GetSingleton());
            holder->AddEventSink<RE::TESHitEvent>(HitSink::GetSingleton());
            Probe::RegisterCrosshairSink();
            spdlog::info("[loadout] hit sink registered (shield restore)");
            spdlog::info("[diag] Field Orders power will dump a state report");
        } else {
            spdlog::error("[diag] no event holder -- power dump unavailable");
        }
    }

    void StopPump() {
        // The guards inside SleeperLoop were dead code while nothing ever
        // cleared this flag, and the "safe across shutdown" comment was
        // therefore fiction. This makes it true.
        g_pumpRunning.store(false);
        g_pumpEpoch.fetch_add(1);   // strand any thread still mid-sleep
    }

    void StartPump() {
        if (g_pumpRunning.exchange(true)) return;   // idempotent across loads
        const auto epoch = g_pumpEpoch.fetch_add(1) + 1;
        spdlog::info("[diag] pump {}ms (evaluator), diagnostics every {}th wake (~{}ms)",
                     kPumpMs, kDiagEveryNth, kPumpMs * kDiagEveryNth);
        std::thread(SleeperLoop, epoch).detach();
    }

}
