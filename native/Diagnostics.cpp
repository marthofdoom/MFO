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
#include "Packages.h"
#include "CasterConsent.h"

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
        // TRUE while an AddTask tick body is actually executing (set AFTER the
        // pump-check, so a tick that starts post-StopPump never sets it). StopPump
        // drains on this so the revert can clear the save-scoped maps without a
        // worker tick inserting mid-clear (audit: concurrent map insert+clear=UB).
        std::atomic<bool>          g_tickActive{ false };
        struct TickActiveGuard {
            TickActiveGuard()  { g_tickActive.store(true); }
            ~TickActiveGuard() { g_tickActive.store(false); }
        };

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

        // MCM WRITES, then closes the Journal Menu; re-read config so settings
        // apply LIVE instead of at next load. MCM Helper persists to
        // Data/MCM/Settings/MFO.ini, which Config::Read ingests last.
        class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            static MenuSink* GetSingleton() { static MenuSink s; return &s; }
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_e,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (a_e && !a_e->opening && a_e->menuName == RE::JournalMenu::MENU_NAME) {
                    SKSE::GetTaskInterface()->AddTask([]() {
                        Config::Read();
                        // Re-apply settings mirrored into derived UI state --
                        // the raw atomics are live, but g_showHud only reaches
                        // the board's g_hud through SetHud. Without this a HUD
                        // toggle in the MCM would not take until next load.
                        Board::SetHud(Config::g_showHud.load());
                        spdlog::info("[config] re-read after Journal/MCM close (HUD {})",
                                     Config::g_showHud.load() ? "on" : "off");
                    });
                }
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

                // A FOLLOWER CASTING IS THE ANIMATION EVIDENCE.
                //
                // The cast path is: spell in hand (Loadout) + a target they are
                // locked onto (Targeting) + the follower's own combat AI firing
                // it -- which is the vanilla path, and therefore animated, with
                // real magicka arbitration. This sink is the only way to tell
                // an AI-fired cast from one MFO issued, and without it Session 6
                // would throw that evidence away (§0.4 proved the sink works;
                // it was simply filtered to the player).
                if (!a_event->object->IsPlayerRef()) {
                    auto* caster = a_event->object->As<RE::Actor>();
                    if (caster && Followers::IsTracked(caster->GetFormID())) {
                        // QUEUE. This reads g_followers, which is main-thread-only
                        // like every other MFO map, and the sink runs on the
                        // event thread (#1). Cost of getting this wrong is a
                        // torn read of the very table the evaluator is using.
                        const auto casterID = caster->GetFormID();
                        const auto spellID  = a_event->spell;
                        SKSE::GetTaskInterface()->AddTask([casterID, spellID]() {
                            auto* actor = RE::TESForm::LookupByID<RE::Actor>(casterID);

                            // Resolve as TESForm, not SpellItem: the field log
                            // printed "?" for the one cast it caught, and "?"
                            // could mean a non-spell form, a nameless spell, or
                            // a failed lookup -- three different stories, and
                            // this is the single most important line in the
                            // session.
                            auto* form = RE::TESForm::LookupByID(spellID);
                            const char* nm = form ? form->GetName() : nullptr;

                            // Was it OURS -- a spell one of their gambits named?
                            // find(), never operator[]: that inserts (#9).
                            bool ours = false;
                            if (const auto rec = g_followers.find(casterID);
                                rec != g_followers.end()) {
                                for (const auto& g : rec->second.combat()) {
                                    if (g.actionParamForm == spellID) { ours = true; break; }
                                }
                            }

                            spdlog::info("[cast] {:08X} {} CAST {} ({:08X}) formType={} -- AI-fired{}",
                                         casterID,
                                         actor && actor->GetName() ? actor->GetName() : "?",
                                         nm && *nm ? nm : "(unnamed)", spellID,
                                         form ? static_cast<int>(form->GetFormType()) : -1,
                                         ours ? "  *** MFO GAMBIT SPELL -- THE ANIMATED PATH ***"
                                              : "  (their own spell, not ours)");

                            // Their AI just cast. Restart the window so MFO does
                            // not double-cast on top of an animation still
                            // playing -- an animated heal takes about a second.
                            // Their AI cast it. That is a real cast -- pace it
                            // exactly like MFO's own, or the limiter only
                            // governs the half of the casting MFO does.
                            if (ours) { Loadout::StartCooldown(casterID); CasterConsent::Clear(casterID); }
                        });
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }
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
                        TickActiveGuard _active;   // marks the tick in-flight for StopPump's drain

                        // Detection and the HUD stay on the old ~532 ms budget;
                        // only the evaluator runs at the deadline.
                        if (diagTurn) Followers::Refresh();

                        Scheduler::Tick();
                        Loadout::Tick();   // hand back stowed two-handers

                        if (diagTurn) Probe::Tick();
                        // The board echoes edits back through the snapshot, so
                        // while it is OPEN publish every tick (133ms) instead of
                        // every 4th -- a half-second echo made DragFloat crawl
                        // and hid whether a click registered (board review M2).
                        if (diagTurn || Board::IsOpen()) Board::PublishSnapshot();
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
            const auto c = CasterConsent::GetStats();
            spdlog::info("  consent hook: {} | {} latched | seen {}, AI-vetoed {}, forced {}",
                         CasterConsent::IsHooked() ? "INSTALLED" : "off",
                         c.latched, c.seen, c.vetoed, c.forced);
        }
        {
            const auto t = Targeting::GetStats();
            spdlog::info("  targeting: hook {} | {} latched | {} assert(s), {} drift(s), {} pass(es){}",
                         Targeting::IsHooked() ? "INSTALLED" : "off",
                         t.latched, t.asserts, t.drifts, t.passes,
                         t.conflictMod ? "  [SmartNPCTargetSelector ALSO LOADED]" : "");
        }
        // WHO IS ACTUALLY IN THE ALIAS.
        //
        // Added because a field test could not distinguish "the alias never
        // filled" from "the alias filled and the package did not run" -- both
        // look like "nothing happened", and a session was spent unable to tell
        // them apart. BGSRefAlias::GetActorReference is a bound reader at the
        // pinned rev, so this costs nothing and ends the ambiguity.
        if (auto* q = Forms::g_commandQuest) {
            spdlog::info("  command quest: {} (priority is a record byte, see 0.25)",
                         q->IsRunning() ? "RUNNING" : "NOT running");
            for (auto* a : q->aliases) {
                auto* ra = a ? skyrim_cast<RE::BGSRefAlias*>(a) : nullptr;
                if (!ra) continue;
                auto* who = ra->GetActorReference();
                spdlog::info("    alias {} '{}': {}", a->aliasID,
                             a->aliasName.empty() ? "?" : a->aliasName.c_str(),
                             who ? std::format("HOLDS {:08X} {}", who->GetFormID(),
                                               who->GetName() ? who->GetName() : "?")
                                 : std::string("empty"));
            }
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
            if (auto* ui = RE::UI::GetSingleton())
                ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
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
        // DRAIN: a tick already past the pump-check may be mid-execution on the
        // job worker (ENGINE_NOTES 0.30), inserting into the save-scoped maps the
        // revert is about to clear. Wait for it to finish so the clear is safe.
        // A tick started after the store above early-returns without setting the
        // flag, so this waits only for the one genuinely in-flight tick and can't
        // deadlock; the cap is a backstop, not an expected path.
        for (int i = 0; g_tickActive.load() && i < 2000; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    void StartPump() {
        if (g_pumpRunning.exchange(true)) return;   // idempotent across loads
        const auto epoch = g_pumpEpoch.fetch_add(1) + 1;
        spdlog::info("[diag] pump {}ms (evaluator), diagnostics every {}th wake (~{}ms)",
                     kPumpMs, kDiagEveryNth, kPumpMs * kDiagEveryNth);
        std::thread(SleeperLoop, epoch).detach();
    }

}
