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
#include "Actuation.h"   // SelfCastReconcile -- the forced self-cast channel lifecycle
#include "Papyrus.h"
#include "Targeting.h"
#include "Packages.h"
#include "CasterConsent.h"
#include "CombatStyle.h"
#include "ProgAllocator.h"   // OnMenuClose — re-read the addon economy on MCM close

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
        // TRUE while an AddTask body that touches save-scoped maps is actually
        // executing. StopPump/PausePump DRAIN on this so a revert/save can act on
        // the maps without a worker body inserting mid-clear (concurrent map
        // insert+clear = UB). See PumpTickGate for the ordering that makes the
        // drain reliable.
        std::atomic<bool>          g_tickActive{ false };
        // Resumable quiesce for SaveCallback (SEV-1 #3): unlike StopPump this
        // leaves g_pumpRunning/g_pumpEpoch untouched (the sleeper thread keeps
        // living), and every gated body bails while it is set. PausePump sets it
        // then drains g_tickActive so the two FLWR passes see a STABLE
        // g_followers; ResumePump clears it.
        std::atomic<bool>          g_pumpPaused{ false };

        // THE ONE guard shape for every MFO AddTask body queued in this file
        // (the sleeper tick and all four sinks). Two jobs:
        //  1. Dekker handshake with StopPump/PausePump. We STORE g_tickActive=
        //     true FIRST, THEN load the pump state -- both seq_cst. StopPump
        //     stores running=false (seq_cst) then loads g_tickActive; with a
        //     total seq_cst order at least one side sees the other, so a body
        //     that passed its check can never slip past the drain and mutate a
        //     map the revert is clearing (the check-then-set TOCTOU this
        //     replaces).
        //  2. Staleness bail. Bodies queued at an earlier epoch (or while the
        //     pump is stopped/paused) run their guard, see the mismatch, and
        //     return without touching game state. The captured epoch is taken at
        //     QUEUE time; the guard compares it at RUN time.
        // Construct it FIRST in the body; test it as a bool; on false, return.
        struct PumpTickGate {
            explicit PumpTickGate(std::uint64_t a_queuedEpoch) {
                g_tickActive.store(true, std::memory_order_seq_cst);
                m_ok = g_pumpRunning.load(std::memory_order_seq_cst) &&
                       !g_pumpPaused.load(std::memory_order_seq_cst) &&
                       g_pumpEpoch.load(std::memory_order_seq_cst) == a_queuedEpoch;
            }
            ~PumpTickGate() { g_tickActive.store(false, std::memory_order_seq_cst); }
            explicit operator bool() const { return m_ok; }
            PumpTickGate(const PumpTickGate&)            = delete;
            PumpTickGate& operator=(const PumpTickGate&) = delete;
        private:
            bool m_ok = false;
        };

        // The epoch a sink captures at QUEUE time, so its deferred body can tell
        // it apart from a body queued before a revert/load swapped the pump.
        std::uint64_t CurrentPumpEpoch() {
            return g_pumpEpoch.load(std::memory_order_seq_cst);
        }

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
                // Off-worker probe: this sink runs on the event thread, so it must
                // NOT walk the live g_active vector (Refresh races it, SEV-1).
                if (!Followers::IsTrackedFast(id)) return RE::BSEventNotifyControl::kContinue;

                // Sinks QUEUE; equipping is engine work (INVARIANTS #1). Capture
                // the pump epoch now; the deferred body runs under PumpTickGate so
                // it bails if a revert/save moved the pump after we queued.
                const auto epoch = CurrentPumpEpoch();
                SKSE::GetTaskInterface()->AddTask([id, epoch]() {
                    PumpTickGate gate(epoch);
                    if (!gate) return;
                    Loadout::OnFollowerHit(id);
                });
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
                    const auto epoch = CurrentPumpEpoch();
                    SKSE::GetTaskInterface()->AddTask([epoch]() {
                        PumpTickGate gate(epoch);   // shared body shape; drains with StopPump
                        if (!gate) return;
                        Config::Read();
                        // Re-apply settings mirrored into derived UI state --
                        // the raw atomics are live, but g_showHud only reaches
                        // the board's g_hud through SetHud. Without this a HUD
                        // toggle in the MCM would not take until next load.
                        Board::SetHud(Config::g_showHud.load());
                        spdlog::info("[config] re-read after Journal/MCM close (HUD {})",
                                     Config::g_showHud.load() ? "on" : "off");
                    });
                    // An optional progression addon may bind MCM GlobalValue
                    // sliders to its economy GLOBs; re-read them so a slider edit
                    // is live now. Generic — the DLL never names any addon; this
                    // is a no-op unless an addon manifest is present. OnMenuClose
                    // marshals to the true main thread itself (g_econ discipline).
                    ProgAllocator::OnMenuClose();
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
                    // Off-worker probe (event thread): the mirror, never a live
                    // g_active walk that Refresh could be reallocating (SEV-1).
                    if (caster && Followers::IsTrackedFast(caster->GetFormID())) {
                        // QUEUE. This reads g_followers, which is main-thread-only
                        // like every other MFO map, and the sink runs on the
                        // event thread (#1). Cost of getting this wrong is a
                        // torn read of the very table the evaluator is using.
                        const auto casterID = caster->GetFormID();
                        const auto spellID  = a_event->spell;
                        const auto epoch    = CurrentPumpEpoch();
                        SKSE::GetTaskInterface()->AddTask([casterID, spellID, epoch]() {
                            // Same shared guard as every body here: the g_followers
                            // find() below must not race the worker's map rebuild,
                            // and this participates in StopPump/PausePump's drain.
                            PumpTickGate gate(epoch);
                            if (!gate) return;
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

                            // Their AI just cast OUR spell. That is a real cast
                            // -- pace it exactly like MFO's own (StartCooldown),
                            // or the limiter only governs the half of the
                            // casting MFO does.
                            //
                            // v1.0.30: the latch is KEPT, not cleared. Clearing
                            // here was the between-casts LEAK: the deny dropped
                            // the instant the cast fired, and until the next
                            // service tick re-Want()ed -- a full cooldown when
                            // the spell got yanked, since the Debounced branch
                            // never re-latched -- his AI was free to slip in
                            // its own Chain Lightning between two gambit casts.
                            // Exclusive control must span the PAUSE between
                            // paced casts; the latch now lives exactly as long
                            // as the cast rule keeps winning, and the scheduler
                            // H3 release / combat end / dismissal end it.
                            // NoteOurCast retires only the per-cast transients
                            // (miss flag, deny-log dedup); it returns false when
                            // the rule already released, so the hold line below
                            // cannot claim control nobody has. One line per
                            // gambit cast -- cast cadence, never tick cadence.
                            if (ours) {
                                Loadout::StartCooldown(casterID);
                                if (CasterConsent::NoteOurCast(casterID)) {
                                    spdlog::info("[consent] {:08X} holding exclusive control "
                                                 "through the cast cooldown", casterID);
                                }
                            }

                            // HYBRID SIGNALS (real SPELLS only -- this sink
                            // also catches potions, §0.16's Ale, and a drink
                            // during the grace is not the AI overruling the
                            // gambit):
                            //  * NoteCast -- if this follower is LATCHED and
                            //    this is NOT the latched spell, the AI went
                            //    its own way; Actuation forces next tick.
                            //  * NotifyCast -- if this is the cast PACKAGE's
                            //    holder firing the commanded spell, the pop
                            //    releases him a moment later (Packages::Pump).
                            if (form && form->Is(RE::FormType::Spell)) {
                                CasterConsent::NoteCast(casterID, spellID);
                                Packages::NotifyCast(casterID, spellID);
                            }
                        });
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!Forms::g_fieldOrders)            return RE::BSEventNotifyControl::kContinue;
                if (a_event->spell != Forms::g_fieldOrders->GetFormID())
                    return RE::BSEventNotifyControl::kContinue;

                // Sinks queue; they never do engine work inline.
                const auto epoch = CurrentPumpEpoch();
                SKSE::GetTaskInterface()->AddTask([epoch]() {
                    // DumpReport / PublishSnapshot read g_followers -- gate them.
                    PumpTickGate gate(epoch);
                    if (!gate) return;
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
                        // Dekker-ordered guard: publish in-flight FIRST, THEN
                        // re-check the pump, so StopPump's drain can't miss a tick
                        // that already passed the old check-then-set (SEV-2
                        // TOCTOU). Also bails if a save PausePump'd or the epoch
                        // moved after this was queued.
                        PumpTickGate gate(a_epoch);
                        if (!gate) return;

                        // Detection and the HUD stay on the old ~532 ms budget;
                        // only the evaluator runs at the deadline.
                        if (diagTurn) Followers::Refresh();

                        Scheduler::Tick();
                        Actuation::SelfCastReconcile();     // release self-cast channels when their rule goes stale (dispel lingering buffs)
                        Actuation::TargetCastReconcile();   // release on-target direct-force streams (heal/damage re-flow; dispel lingering ward)
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
        spdlog::info("  weapon-style: {} | {} follower(s) own a stance",
                     Config::g_weaponStyleControl.load() ? "ON" : "off",
                     CombatStyle::OwnedCount());
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
        // Dekker pairing with PumpTickGate: STORE running=false (seq_cst) BEFORE
        // loading g_tickActive below. The gate stores tickActive=true (seq_cst)
        // before loading running; under the single seq_cst total order at least
        // one side observes the other, so the drain cannot miss a body that just
        // passed its check (the TOCTOU this rework closes).
        g_pumpRunning.store(false, std::memory_order_seq_cst);
        g_pumpEpoch.fetch_add(1, std::memory_order_seq_cst);   // strand any thread still mid-sleep
        // DRAIN: a tick already past the pump-check may be mid-execution on the
        // job worker (ENGINE_NOTES 0.30), inserting into the save-scoped maps the
        // revert is about to clear. Wait for it to finish so the clear is safe.
        // A tick started after the store above early-returns without setting the
        // flag, so this waits only for the one genuinely in-flight tick and can't
        // deadlock; the cap is a backstop, not an expected path.
        int waited = 0;
        for (; g_tickActive.load(std::memory_order_seq_cst) && waited < 2000; ++waited)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // The cap is a backstop, never an expected path. If it TRIPS, the clears
        // that follow race the still-live tick's map inserts (the unordered_map UB
        // this StopPump rework was built to prevent) -- so make it LOUD rather than
        // silent, per the Fable audit (#9). A crash right after a load-screen would
        // point straight here.
        if (g_tickActive.load(std::memory_order_seq_cst))
            spdlog::error("[diag] StopPump: worker tick STILL active after {}ms cap -- revert "
                          "clears may race its inserts (audit #9); a post-load crash starts here", waited);
    }

    void PausePump() {
        // Resumable quiesce for SaveCallback (SEV-1 #3). Unlike StopPump it does
        // NOT touch g_pumpRunning/g_pumpEpoch -- the sleeper thread keeps living
        // and StartPump is not needed to resume. It sets g_pumpPaused (which every
        // PumpTickGate observes and bails on), then DRAINS the one body that may
        // already be past its gate mutating a save-scoped map. After this returns,
        // g_followers is stable for the callback's count+write passes. Same Dekker
        // handshake as StopPump: store paused (seq_cst) before loading tickActive.
        g_pumpPaused.store(true, std::memory_order_seq_cst);
        int waited = 0;
        for (; g_tickActive.load(std::memory_order_seq_cst) && waited < 2000; ++waited)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (g_tickActive.load(std::memory_order_seq_cst))
            spdlog::error("[diag] PausePump: worker body STILL active after {}ms cap -- "
                          "SaveCallback may race its g_followers mutation", waited);
    }

    void ResumePump() {
        g_pumpPaused.store(false, std::memory_order_seq_cst);
    }

    void StartPump() {
        if (g_pumpRunning.exchange(true)) return;   // idempotent across loads
        const auto epoch = g_pumpEpoch.fetch_add(1) + 1;
        spdlog::info("[diag] pump {}ms (evaluator), diagnostics every {}th wake (~{}ms)",
                     kPumpMs, kDiagEveryNth, kPumpMs * kDiagEveryNth);
        std::thread(SleeperLoop, epoch).detach();
    }

}
