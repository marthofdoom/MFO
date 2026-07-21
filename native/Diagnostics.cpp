#include "PCH.h"
#include "Diagnostics.h"
#include "Followers.h"
#include "Rapport.h"
#include "Config.h"
#include "Forms.h"
#include "State.h"
#include "Board.h"

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

        // 2s was fine for detection-only logging. The HUD shows live vitals, so
// it needs to be quick enough to read as live without being a tick loop.
constexpr std::uint32_t kRefreshMs = 500;
        std::atomic<bool> g_pumpRunning{ false };

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
        void SleeperLoop() {
            while (g_pumpRunning.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kRefreshMs));
                if (!g_pumpRunning.load()) break;
                // Null-check: on shutdown the interface can be gone while
                // this thread is still awake.
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() {
                        if (!g_pumpRunning.load()) return;
                        Followers::Refresh();
                        // Republish every tick so the passive HUD stays live
                        // during combat without anything being opened.
                        Board::PublishSnapshot();
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
    }

    void StartPump() {
        if (g_pumpRunning.exchange(true)) return;   // idempotent across loads
        spdlog::info("[diag] detection refresh every {}ms", kRefreshMs);
        std::thread(SleeperLoop).detach();
    }

}
