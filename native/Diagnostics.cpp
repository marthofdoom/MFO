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
#include "ComposedCast.h" // CFC observe hand-off: an executor-armed cast landing is THE animated path
#include "APMFBridge.h"  // Phase 3: per-pump auto-release of APMF cast-select claims
#include "Papyrus.h"
#include "Targeting.h"
#include "Packages.h"
#include "CasterConsent.h"
#include "CombatStyle.h"
#include "ProgAllocator.h"   // OnMenuClose — re-read the addon economy on MCM close
#include "MainThread.h"      // [atk-obs]: graph sink attach/detach is main-thread work (#62-class)

// Two Win32 symbols, declared by hand. <windows.h> is BANNED outside Board.cpp
// (it #defines GetObject and hijacks BGSDefaultObjectManager::GetObject<T>,
// ENGINE_NOTES §9 -- the same reason APMFBridge/Targeting/Logistics hand-declare
// GetModuleHandleA). void* is ABI-correct for HMODULE on Win64; DWORD is
// unsigned long. The real header is never in this TU to conflict.
extern "C" __declspec(dllimport) void*         __stdcall GetModuleHandleA(const char* a_name);
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId();
// [fatal] passive hook (2026-09-21), same rule. AddVectoredExceptionHandler(ULONG
// First, PVECTORED_EXCEPTION_HANDLER) -> PVOID; the handler is LONG NTAPI
// (EXCEPTION_POINTERS*), taken as void* here and cast inside. GetModuleHandleExA
// (DWORD, LPCSTR, HMODULE*) -> BOOL (int). See the [fatal] block below.
extern "C" __declspec(dllimport) void*         __stdcall AddVectoredExceptionHandler(unsigned long a_first, long(__stdcall* a_handler)(void*));
extern "C" __declspec(dllimport) int           __stdcall GetModuleHandleExA(unsigned long a_flags, const char* a_addr, void** a_out);

#include <cstdio>       // [fatal]: snprintf for the module+rva text (no allocation in the handler)
#include <cstdlib>      // [fatal]: std::abort
#include <exception>    // [fatal]: std::set_terminate

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

        // [hb] HEARTBEAT (2026-09-21, bHeartbeat, default ON). One line per 5 s
        // from the SLEEPER thread -- MFO's own thread, which only sleeps and
        // AddTasks, so it keeps printing while everything else hangs:
        //   [hb] worker ticks=N main drains=M mainAge=<ms> lastWorkerStage='<s>'
        // ticks: AddTask bodies that ran (the job worker). drains: heartbeat
        // probes that ran on the MAIN thread (one MainThread::Post per line;
        // the count freezes when the player's Update stops). mainAge: ms since
        // the last probe ran. lastWorkerStage: the step the worker body was in
        // when it last wrote the stage -- on a hung worker it names the callee
        // that never returned. All relaxed atomics; the stage is a pointer to a
        // string literal (never freed). Not save-scoped: a session count.
        constexpr std::uint32_t kHeartbeatMs = 5000;
        std::atomic<std::uint64_t> g_hbWorkerTicks{ 0 };
        std::atomic<std::uint64_t> g_hbMainDrains{ 0 };
        std::atomic<std::int64_t>  g_hbMainLastMs{ 0 };
        std::atomic<const char*>   g_hbWorkerStage{ "never" };
        inline void HbStage(const char* a_stage) noexcept { g_hbWorkerStage.store(a_stage, std::memory_order_relaxed); }
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
        // PumpTickGate + CurrentPumpEpoch are now DECLARED in Diagnostics.h (the
        // SEV-1 wave exposed them to the cross-TU sinks in Rapport/Logistics/
        // Board) and DEFINED out-of-line below, after this anonymous namespace,
        // so the pump atomics above stay file-local while the class is shared.

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

                            // Was it OURS -- a spell one of their gambits named, OR
                            // (cast-claim observability, 2026-09-06) the delivery-flip
                            // PROXY of a LIVE claim ComposedCast is watching? A claim
                            // whose delivery APMF flipped through its own minted proxy
                            // (APMF_API::APMF_API_v6::GetCastProxy) lands as a cast of
                            // the PROXY FormID, never the gambit's configured
                            // actionParamForm, so the actionParamForm-only check below
                            // filed that landing as "their own spell, not ours" even
                            // though it was our own claimed cast firing (the exact false
                            // alarm diagnosed 2026-09-06). ExpectingCast now matches
                            // spell OR proxy internally (ComposedCast.h/.cpp) -- reuse it
                            // here instead of re-deriving the proxy match, and reuse the
                            // SAME bool below for the CFC-specific hand-off so this only
                            // calls it once. find(), never operator[]: that inserts (#9).
                            bool ours = false;
                            if (const auto rec = g_followers.find(casterID);
                                rec != g_followers.end()) {
                                for (const auto& g : rec->second.combat()) {
                                    if (g.actionParamForm == spellID) { ours = true; break; }
                                }
                            }
                            const bool expectingCfc = ComposedCast::ExpectingCast(casterID, spellID);
                            ours = ours || expectingCfc;

                            spdlog::info("[cast] {:08X} {} CAST {} ({:08X}) formType={} -- AI-fired{}",
                                         casterID,
                                         actor && actor->GetName() ? actor->GetName() : "?",
                                         nm && *nm ? nm : "(unnamed)", spellID,
                                         form ? static_cast<int>(form->GetFormType()) : -1,
                                         ours ? "  *** MFO GAMBIT SPELL -- THE ANIMATED PATH ***"
                                              : "  (their own spell, not ours)");

                            // COMPOSED FORCED CAST observe hand-off (SPEC-FORCED-CAST.md
                            // §1.3 RELEASE / §5). If the executor armed an "expected
                            // cast" for this (follower, spell) just before it triggered,
                            // THIS event is the executor's own animated cast landing --
                            // the positive fire signal its RELEASE phase waits on.
                            if (expectingCfc) {
                                ComposedCast::NoteObservedCast(casterID, spellID);
                                spdlog::info("[cast] {:08X} {} CFC-fired {} ({:08X}) "
                                             "*** THE ANIMATED PATH ***", casterID,
                                             actor && actor->GetName() ? actor->GetName() : "?",
                                             nm && *nm ? nm : "(unnamed)", spellID);
                            }

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

        // ═══════════════════════════════════════════════════════════════════════
        // [atk-obs] PASSIVE ATTACK-EVENT PROBE (feat/mfo-attack-observe, 2026-09-21)
        // ═══════════════════════════════════════════════════════════════════════
        // Observation only. Answers ONE field question -- which attack moves does a
        // managed follower actually play in a fight, and how long does he stand in
        // reach with weapons drawn doing nothing -- so the dual-wield "same move
        // repeatedly" complaint can be diagnosed from a histogram instead of a
        // theory (the ER-DW-Sword override theory predicts SCAR_ComboStart=0,
        // BFCO_NextIsAttackN=0, MCO_WinOpen~=attacks, power>=normal).
        //
        // Mechanism (zero hooks, design log step 7, every symbol pinned 3.7.0):
        //   actor->GetAnimationGraphManager(mgr)            IAnimationGraphManagerHolder.h:44
        //   mgr->graphs[i]  (BSTSmallArray<BSTSmartPointer<BShkbAnimationGraph>>)  BSAnimationGraphManager.h:121
        //   BShkbAnimationGraph : BSTEventSource<BSAnimationGraphEvent>  (+0x68)  BShkbAnimationGraph.h:32
        //   BSTEventSource::AddEventSink / RemoveEventSink  (spin-locked, deduped) BSTEvent.h:34 / :64
        //   BSAnimationGraphEvent { tag, holder (const TESObjectREFR*), payload }  BSAnimationGraphEvent.h:13-15
        //
        // THREADING CONTRACT -- the sink runs on WHATEVER thread the graph
        // dispatches on (unknown until the field says; the "sink thread=" line
        // below is principle 5: observe it). Therefore the sink:
        //   * touches ONLY fixed-size atomics (a per-slot counter row, an unknown-
        //     tag table with CAS-claimed entries) -- no map, no lock, no allocation,
        //     no spdlog, no engine call, no roster (g_followers/g_active never; the
        //     slot's fid atomic is the membership test, #4/#74);
        //   * reads the event's holder ONLY for its FormID (a plain field of the
        //     REFR that is dispatching -- alive by construction);
        //   * never sends an event, never NotifyAnimationGraph, never mutates the
        //     graph. Attach/detach are MainThread::Post'd (graph mutation is
        //     main-thread work, #62-class), capture the FormID + slot only, and
        //     re-resolve + null-check on the main thread.
        // The WORKER (sleeper tick, under PumpTickGate) owns everything else: slot
        // assignment, fight open/close, the idle-in-reach accumulator (read from the
        // actor on the worker, never from the sink), the dump/reset, and every log
        // line. Counter reset is exchange(0): an increment landing between the
        // worker's exchange and its line is counted into the NEXT fight -- an
        // off-by-one at a fight boundary, accepted and documented.

        constexpr std::size_t kAtkSlots   = 16;   // managed followers observed at once
        constexpr std::size_t kAtkUnknown = 32;   // session-wide unknown-tag table
        constexpr std::size_t kAtkText    = 72;   // tag[<=40] + '|' + payload[<=24] + NUL
        constexpr std::size_t kAtkName    = 40;

        // Exact tags (case-insensitive compare: the engine's string cache is
        // case-insensitive and c_str() returns whichever spelling interned first).
        constexpr const char* kAtkTags[] = {
            "attackStart", "attackStartLeftHand", "attackStartDualWield",
            "attackPowerStartInPlace", "attackPowerStartForward", "attackPowerStartBackward",
            "attackPowerStartLeft", "attackPowerStartRight", "attackPowerStartDualWield",
            "bashStart", "bashPowerStart", "attackStop", "attackRelease",
            "weaponSwing", "weaponLeftSwing", "HitFrame", "preHitFrame",
            "staggerStart", "recoilStart", "blockStart", "blockStop", "bleedOutStart",
            "MCO_WinOpen", "MCO_WinClose", "MCO_PowerWinOpen", "MCO_PowerWinClose",
            "BFCO_NextWinStart", "BFCO_NextPowerWinStart",
            "BFCO_NextIsAttack1", "BFCO_NextIsAttack2", "BFCO_NextIsAttack3",
            "BFCO_NextIsAttack4", "BFCO_NextIsAttack5", "BFCO_NextIsAttack6",
            "BFCO_NextIsAttack7", "BFCO_NextIsAttack8", "BFCO_NextIsAttack9",
            "BFCO_NextIsPowerAttack1", "BFCO_NextIsPowerAttack2", "BFCO_NextIsPowerAttack3",
            "BFCO_NextIsPowerAttack4", "BFCO_NextIsPowerAttack5", "BFCO_NextIsPowerAttack6",
            "BFCO_NextIsPowerAttack7", "BFCO_NextIsPowerAttack8", "BFCO_NextIsPowerAttack9",
            "BFCO_DIY_EndLoop", "BFCO_MoveStart", "SCAR_ComboStart", "SCAR_UpdateDummy",
        };
        constexpr std::size_t kAtkExact = std::size(kAtkTags);
        // Prefix families, tested in THIS order after the exact list misses
        // (SkyParkour before Parkour). A family hit also lands in the unknown-tag
        // table so the dump names the exact tag; PIE entries carry the payload's
        // first 24 chars (Payload Interpreter's graph-variable writes).
        constexpr const char* kAtkFamilies[]      = { "BFCO_",  "MCO_",  "SCAR_",  "PIE",  "SkyParkour",  "Parkour"  };
        constexpr const char* kAtkFamilyLabels[]  = { "BFCO_*", "MCO_*", "SCAR_*", "PIE*", "SkyParkour*", "Parkour*" };
        constexpr std::size_t kAtkFam      = std::size(kAtkFamilies);
        constexpr std::size_t kAtkFamPIE   = 3;
        constexpr std::size_t kAtkCounters = kAtkExact + kAtkFam;
        constexpr std::int64_t kAtkIdleMs  = 1500;   // an idle-in-reach run longer than this counts
        constexpr std::int64_t kAtkOocMs   = 1500;   // IsInCombat low for this long closes the fight
        constexpr std::int64_t kAtkAttachMs = 2000;  // re-post the (deduped) attach this often while managed + 3D-loaded (in or out of combat)

        constexpr char AtkLower(char a_c) noexcept { return (a_c >= 'A' && a_c <= 'Z') ? static_cast<char>(a_c + 32) : a_c; }
        bool AtkEq(const char* a_a, const char* a_b) noexcept {
            for (;; ++a_a, ++a_b) {
                if (AtkLower(*a_a) != AtkLower(*a_b)) return false;
                if (!*a_a) return true;
            }
        }
        bool AtkPrefix(const char* a_s, const char* a_prefix) noexcept {
            for (; *a_prefix; ++a_s, ++a_prefix)
                if (AtkLower(*a_s) != AtkLower(*a_prefix)) return false;
            return true;
        }
        void AtkCopy(char* a_dst, std::size_t a_cap, const char* a_src, std::size_t a_max) noexcept {
            std::size_t n = 0;
            if (a_src) for (; n + 1 < a_cap && n < a_max && a_src[n]; ++n) a_dst[n] = a_src[n];
            a_dst[n] = '\0';
        }
        std::int64_t AtkNowMs() noexcept {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        struct AtkSlot {
            // ── sink-written (atomics only) ──
            std::atomic<RE::FormID>     fid{ 0 };                  // 0 = free; the sink's membership test
            std::atomic<std::uint32_t>  count[kAtkCounters]{};
            std::atomic<std::uint32_t>  other[kAtkUnknown]{};
            std::atomic<std::uint32_t>  otherOverflow{ 0 };        // unknown-tag events with no table entry left
            std::atomic<std::int64_t>   lastAttackMs{ 0 };         // steady ms of the last counted event
            std::atomic<bool>           attachLogged{ false };     // main-thread attach line, once per assignment
            // ── worker-only ──
            bool          fightOpen    = false;
            std::uint32_t fightNo      = 0;
            std::int64_t  fightStartMs = 0;
            bool          oocArmed     = false;
            std::int64_t  oocSinceMs   = 0;
            bool          idleRunning  = false;
            std::int64_t  idleRunStart = 0;
            std::int64_t  lastAttachMs = 0;    // attach re-post throttle while a fight is open (SEV-3)
            std::uint32_t idleCount    = 0;
            std::int64_t  idleLongest  = 0;
            char          name[kAtkName]{};
            char          weapR[kAtkName]{};
            char          weapL[kAtkName]{};
        };
        AtkSlot g_atk[kAtkSlots];

        // Session-wide unknown-tag table. state: 0 free, 1 being written (skip),
        // 2 ready + unlogged, 3 ready + logged. Text is written under the CAS
        // claim and published with a release-store of 2; readers acquire-load
        // state before touching text. Two graphs dispatching the same never-seen
        // tag concurrently can claim two entries with one text -- a duplicate row
        // in the dump, cosmetic, accepted.
        struct AtkUnk {
            std::atomic<int>        state{ 0 };
            std::atomic<RE::FormID> firstFid{ 0 };
            char                    text[kAtkText]{};
        };
        AtkUnk g_atkUnk[kAtkUnknown];

        // "sink thread=" evidence: 0 unseen, 1 claiming, 2 recorded + unlogged, 3 logged.
        std::atomic<int>           g_atkThreadState{ 0 };
        unsigned long              g_atkSinkThread = 0;
        std::atomic<unsigned long> g_atkMainThread{ 0 };
        char                       g_atkFirstTag[kAtkName]{};
        std::atomic<bool>          g_atkSlotsFullLogged{ false };
        bool                       g_atkNoPumpLogged = false;   // worker-only

        void AtkNoteUnknown(AtkSlot& a_s, RE::FormID a_fid, const char* a_text) noexcept {
            for (std::size_t i = 0; i < kAtkUnknown; ++i) {
                auto& e = g_atkUnk[i];
                if (e.state.load(std::memory_order_acquire) >= 2 && AtkEq(e.text, a_text)) {
                    a_s.other[i].fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            for (std::size_t i = 0; i < kAtkUnknown; ++i) {
                auto& e   = g_atkUnk[i];
                int   exp = 0;
                if (e.state.compare_exchange_strong(exp, 1, std::memory_order_acq_rel)) {
                    AtkCopy(e.text, kAtkText, a_text, kAtkText - 1);
                    e.firstFid.store(a_fid, std::memory_order_relaxed);
                    e.state.store(2, std::memory_order_release);
                    a_s.other[i].fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            a_s.otherOverflow.fetch_add(1, std::memory_order_relaxed);
        }

        // One sink object per slot, registered ONLY on that slot's follower's
        // graphs, so an event reaching sink[i] is by construction from follower i
        // -- and the fid compare below closes the slot-reuse window (a stale
        // registration from a previous occupant's graph fails it). Static storage:
        // a registered sink pointer never dangles even if RemoveEventSink is
        // never reached (actor unloaded, graph destroyed with it).
        class AttackSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
        public:
            std::size_t slot = 0;

            RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_ev,
                                                  RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override {
                if (!a_ev || !a_ev->holder) return RE::BSEventNotifyControl::kContinue;
                if (!Config::g_attackObserve.load(std::memory_order_relaxed))
                    return RE::BSEventNotifyControl::kContinue;
                auto&      s   = g_atk[slot];
                const auto fid = s.fid.load(std::memory_order_acquire);
                if (fid == 0 || a_ev->holder->GetFormID() != fid) return RE::BSEventNotifyControl::kContinue;
                const char* tag = a_ev->tag.c_str();
                if (!tag || !*tag) return RE::BSEventNotifyControl::kContinue;

                // Principle 5: WHERE do these arrive. Once per session, no logging
                // here -- the worker prints it.
                if (g_atkThreadState.load(std::memory_order_acquire) == 0) {
                    int exp = 0;
                    if (g_atkThreadState.compare_exchange_strong(exp, 1, std::memory_order_acq_rel)) {
                        g_atkSinkThread = ::GetCurrentThreadId();
                        AtkCopy(g_atkFirstTag, kAtkName, tag, kAtkName - 1);
                        g_atkThreadState.store(2, std::memory_order_release);
                    }
                }

                for (std::size_t i = 0; i < kAtkExact; ++i) {
                    if (AtkEq(tag, kAtkTags[i])) {
                        s.count[i].fetch_add(1, std::memory_order_relaxed);
                        s.lastAttackMs.store(AtkNowMs(), std::memory_order_relaxed);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                }
                std::size_t fam = kAtkFam;
                for (std::size_t j = 0; j < kAtkFam; ++j) {
                    if (AtkPrefix(tag, kAtkFamilies[j])) { fam = j; break; }
                }
                if (fam < kAtkFam) {
                    s.count[kAtkExact + fam].fetch_add(1, std::memory_order_relaxed);
                    s.lastAttackMs.store(AtkNowMs(), std::memory_order_relaxed);   // any custom action tag resets the idle clock
                }
                // Unknown-tag table (bounded). Footstep tags are pure locomotion
                // noise and would fill it on the first stride; SoundPlay.* has
                // dozens of spellings and collapses to one row.
                if (AtkPrefix(tag, "Foot")) return RE::BSEventNotifyControl::kContinue;
                char text[kAtkText];
                if (AtkPrefix(tag, "SoundPlay.")) {
                    AtkCopy(text, kAtkText, "SoundPlay.*", kAtkText - 1);
                } else {
                    AtkCopy(text, kAtkText, tag, 40);
                    if (fam == kAtkFamPIE) {
                        const char* pl = a_ev->payload.c_str();
                        if (pl && *pl) {
                            std::size_t n = 0;
                            while (text[n]) ++n;
                            text[n++] = '|';
                            AtkCopy(text + n, kAtkText - n, pl, 24);
                        }
                    }
                }
                AtkNoteUnknown(s, fid, text);
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        AttackSink g_atkSinks[kAtkSlots];

        // Attach / detach: MAIN thread (graph mutation). FormID + slot captured;
        // re-resolve, null-check. AddEventSink dedupes under its spin lock, so
        // re-posting every 2 s while the follower is managed + 3D-loaded (graphs are rebuilt on a 3D
        // reload, in or out of combat) is idempotent.
        void AtkPostAttach(RE::FormID a_fid, std::size_t a_slot) {
            if (!MainThread::IsInstalled()) {
                if (!g_atkNoPumpLogged) {
                    g_atkNoPumpLogged = true;
                    spdlog::warn("[atk-obs] main-thread pump not installed (VR?) -- graph sinks NOT attached, histogram stays empty");
                }
                return;
            }
            MainThread::Post([a_fid, a_slot]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_fid);
                if (!actor) return;
                if (g_atk[a_slot].fid.load(std::memory_order_acquire) != a_fid) return;   // slot moved on
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
                if (!actor->GetAnimationGraphManager(mgr) || !mgr) return;
                g_atkMainThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
                int n = 0;
                for (auto& g : mgr->graphs) {
                    if (!g) continue;
                    // BShkbAnimationGraph carries TWO BSTEventSource bases; name the one we mean.
                    static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(g.get())->AddEventSink(&g_atkSinks[a_slot]);
                    ++n;
                }
                if (!g_atk[a_slot].attachLogged.exchange(true, std::memory_order_acq_rel)) {
                    spdlog::info("[atk-obs] {:08X} sink attached to {} animation graph(s) (slot {})", a_fid, n, a_slot);
                } else {
                    spdlog::debug("[atk-obs] {:08X} sink re-attached to {} graph(s)", a_fid, n);
                }
            });
        }
        void AtkPostDetach(RE::FormID a_fid, std::size_t a_slot) {
            if (!MainThread::IsInstalled()) return;
            MainThread::Post([a_fid, a_slot]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_fid);
                if (!actor) return;
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
                if (!actor->GetAnimationGraphManager(mgr) || !mgr) return;
                for (auto& g : mgr->graphs) {
                    if (!g) continue;
                    static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(g.get())->RemoveEventSink(&g_atkSinks[a_slot]);
                }
            });
        }

        // Close an idle-in-reach run of a_lenMs (worker).
        void AtkCloseIdle(AtkSlot& a_s, std::int64_t a_lenMs) noexcept {
            if (a_lenMs > kAtkIdleMs) {
                ++a_s.idleCount;
                if (a_lenMs > a_s.idleLongest) a_s.idleLongest = a_lenMs;
            }
        }

        // THE LINE (worker). Only non-zero counters. Resets the fight.
        void AtkDump(std::size_t a_slot, const char* a_why) {
            auto& s = g_atk[a_slot];
            if (!s.fightOpen) return;
            const auto now = AtkNowMs();
            if (s.idleRunning) {
                if (const auto last = s.lastAttackMs.load(std::memory_order_relaxed); last > s.idleRunStart) {
                    AtkCloseIdle(s, last - s.idleRunStart);
                    s.idleRunStart = last;
                }
                AtkCloseIdle(s, now - s.idleRunStart);
                s.idleRunning = false;
            }
            const auto fid = s.fid.load(std::memory_order_acquire);
            std::string line = std::format("[atk-obs] {:08X} '{}' fight#{} {:.1f}s R='{}' L='{}' |",
                                           fid, static_cast<const char*>(s.name), s.fightNo,
                                           (now - s.fightStartMs) / 1000.0,
                                           static_cast<const char*>(s.weapR), static_cast<const char*>(s.weapL));
            bool any = false;
            for (std::size_t i = 0; i < kAtkCounters; ++i) {
                const auto n = s.count[i].exchange(0, std::memory_order_relaxed);
                if (!n) continue;
                any = true;
                line += std::format(" {}={}", i < kAtkExact ? kAtkTags[i] : kAtkFamilyLabels[i - kAtkExact], n);
            }
            if (!any) line += " (no attack events)";
            line += std::format(" | idle-in-reach>{}ms x{} longest={}ms", kAtkIdleMs, s.idleCount, s.idleLongest);
            // "other": the TOP-8 unknown/family tags by count this fight (the table
            // holds up to 32; the line names the 8 that mattered and counts the rest).
            std::pair<std::uint32_t, std::size_t> others[kAtkUnknown];
            std::size_t nOthers = 0;
            for (std::size_t i = 0; i < kAtkUnknown; ++i) {
                const auto n = s.other[i].exchange(0, std::memory_order_relaxed);
                if (n) others[nOthers++] = { n, i };
            }
            std::sort(others, others + nOthers, [](const auto& a, const auto& b) { return a.first > b.first; });
            bool anyOther = false;
            for (std::size_t k = 0; k < nOthers && k < 8; ++k) {
                line += anyOther ? "," : " | other: ";
                anyOther = true;
                const auto  i     = others[k].second;
                const bool  ready = g_atkUnk[i].state.load(std::memory_order_acquire) >= 2;
                line += std::format("{}={}", ready ? static_cast<const char*>(g_atkUnk[i].text) : "?", others[k].first);
            }
            if (nOthers > 8) line += std::format(",+{} more", nOthers - 8);
            if (const auto ov = s.otherOverflow.exchange(0, std::memory_order_relaxed))
                line += std::format("{}other-overflow={}", anyOther ? "," : " | other: ", ov);
            line += std::format(" [{}]", a_why);
            spdlog::info("{}", line);
            s.fightOpen   = false;
            s.oocArmed    = false;
            s.idleCount   = 0;
            s.idleLongest = 0;
        }

        void AtkClearSlot(AtkSlot& a_s) noexcept {
            a_s.fid.store(0, std::memory_order_release);
            for (auto& c : a_s.count) c.store(0, std::memory_order_relaxed);
            for (auto& c : a_s.other) c.store(0, std::memory_order_relaxed);
            a_s.otherOverflow.store(0, std::memory_order_relaxed);
            a_s.lastAttackMs.store(0, std::memory_order_relaxed);
            a_s.attachLogged.store(false, std::memory_order_relaxed);
            a_s.fightOpen = false; a_s.fightNo = 0; a_s.fightStartMs = 0;
            a_s.oocArmed = false; a_s.oocSinceMs = 0;
            a_s.idleRunning = false; a_s.idleRunStart = 0; a_s.idleCount = 0; a_s.idleLongest = 0;
            a_s.lastAttachMs = 0;
            a_s.name[0] = a_s.weapR[0] = a_s.weapL[0] = '\0';
        }

        // Per-follower service (worker, every pump wake). Pure reads of the actor.
        void AtkService(std::size_t a_slot, RE::Actor* a_actor, std::int64_t a_nowMs) {
            auto&      s   = g_atk[a_slot];
            const auto fid = s.fid.load(std::memory_order_acquire);
            // ATTACH whenever the follower is managed and 3D-loaded, in OR out of
            // combat (2026-09-21, Deck/Fable: a non-fighting follower's parkour /
            // stuck tags never reached the sink because it was only attached at
            // fight open), and KEEP attaching on the 2 s throttle: a graph rebuilt
            // (catch-up teleport through disable/enable, a door, a 3D reload)
            // would otherwise leave a fight open with NO registration and print
            // "(no attack events)" -- a masked failure (principle 7; Fable SEV-3
            // on 2ac4163). AddEventSink dedupes under its spin lock
            // (BSTEvent.h:41-51), so a re-post costs one LookupByID + one lock
            // per graph per 2 s per follower on the main thread; the re-attach
            // line stays at debug. Out-of-combat events land in the slot's
            // counters and the session-wide new-tag table (the `[atk-obs]
            // new-tag` first-sight line is how they surface); the counters are
            // zeroed at fight open below, so the per-fight histogram stays
            // fight-scoped.
            if (a_actor->Is3DLoaded() && a_nowMs - s.lastAttachMs >= kAtkAttachMs) {
                s.lastAttachMs = a_nowMs;
                AtkPostAttach(fid, a_slot);
            }
            if (a_actor->IsInCombat()) {
                s.oocArmed = false;
                if (!s.fightOpen) {
                    s.fightOpen    = true;
                    ++s.fightNo;
                    s.fightStartMs = a_nowMs;
                    s.idleRunning  = false;
                    s.idleCount    = 0;
                    s.idleLongest  = 0;
                    // Fight-scoped counters: drop whatever the always-attached sink
                    // counted out of combat since the last dump (same exchange(0)
                    // race window as the dump's own reset -- <=1 event, accepted).
                    for (auto& c : s.count) c.store(0, std::memory_order_relaxed);
                    for (auto& c : s.other) c.store(0, std::memory_order_relaxed);
                    s.otherOverflow.store(0, std::memory_order_relaxed);
                    s.lastAttackMs.store(0, std::memory_order_relaxed);
                }
                // Hands as of the latest in-combat tick (a bow<->melee swap mid-fight
                // shows the final pair; the [equip] lines carry the history).
                auto* r = a_actor->GetEquippedObject(false);
                auto* l = a_actor->GetEquippedObject(true);
                AtkCopy(s.weapR, kAtkName, r && r->GetName() ? r->GetName() : "-", kAtkName - 1);
                AtkCopy(s.weapL, kAtkName, l && l->GetName() ? l->GetName() : "-", kAtkName - 1);
                // IDLE-IN-REACH: weapons drawn, attack state none, within melee reach
                // of the CURRENT combat target. Read from the actor here on the
                // worker (the kActPowerAttack range gate reads distance the same
                // way on this same tick), never from the sink. A counted attack
                // event (lastAttackMs) ends a run at the event's own time.
                // The compare is STRICT (Fable SEV-2 on 2ac4163: `>=` re-closed the
                // run as 0 ms on every tick after a restart, so "attacks once then
                // stands in reach 10 s" printed x0). The three cases, re-derived:
                //   1. an event at A restarts the run at A; later ticks read last==A,
                //      A > A is false, the run stays OPEN and keeps accumulating;
                //   2. a new event at B > A: closes B-A, restarts at B;
                //   3. the break (leaves reach / sheathes / attacks) with last==B:
                //      closes now-B. If an event C > B landed between the last tick
                //      and the break, it closes C-B THEN now-C;
                //   4. combat ends with the run open: closed at the FIRST out-of-
                //      combat tick (the OOC arm site below), never at the dump, so
                //      the 1.5 s close debounce is not counted as idle time.
                const auto* st    = a_actor->AsActorState();
                const bool  drawn = st->IsWeaponDrawn();
                const bool  none  = st->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone;
                auto*       tgt   = a_actor->GetActorRuntimeData().currentCombatTarget.get().get();
                const bool  reach = tgt && a_actor->GetPosition().GetDistance(tgt->GetPosition()) <= Config::g_meleeReach.load();
                if (drawn && none && reach) {
                    if (!s.idleRunning) {
                        s.idleRunning  = true;
                        s.idleRunStart = a_nowMs;
                    } else if (const auto last = s.lastAttackMs.load(std::memory_order_relaxed); last > s.idleRunStart) {
                        AtkCloseIdle(s, last - s.idleRunStart);
                        s.idleRunStart = last;   // the run restarts at the attack
                    }
                } else if (s.idleRunning) {
                    const auto last = s.lastAttackMs.load(std::memory_order_relaxed);
                    if (last > s.idleRunStart) {
                        AtkCloseIdle(s, last - s.idleRunStart);   // the segment before the event
                        s.idleRunStart = last;
                    }
                    AtkCloseIdle(s, a_nowMs - s.idleRunStart);    // the trailing segment
                    s.idleRunning = false;
                }
                return;
            }
            if (!s.fightOpen) return;
            if (!s.oocArmed) {
                s.oocArmed   = true;
                s.oocSinceMs = a_nowMs;
                // Case 4 -- combat ends with the run open (Fable SEV-3 on fc386cf):
                // close it NOW, not at the dump 1.5 s later, or the OOC debounce is
                // added to the trailing segment and every melee kill (drawn, kNone,
                // in reach right after the killing blow) prints a >=1500 ms run.
                // Same pre-event/trailing two-step as the in-combat break. A flap
                // that re-enters combat inside the debounce simply restarts the run
                // on the next in-combat tick. AtkDump's close stays as the belt for
                // the explicit/dismissal path (a no-op once idleRunning is false).
                if (s.idleRunning) {
                    const auto last = s.lastAttackMs.load(std::memory_order_relaxed);
                    if (last > s.idleRunStart) {
                        AtkCloseIdle(s, last - s.idleRunStart);
                        s.idleRunStart = last;
                    }
                    AtkCloseIdle(s, a_nowMs - s.idleRunStart);
                    s.idleRunning = false;
                }
            } else if (a_nowMs - s.oocSinceMs >= kAtkOocMs) {
                AtkDump(a_slot, "combat end");
            }
        }

        void AtkReleaseSlot(std::size_t a_slot, const char* a_why) {
            auto&      s   = g_atk[a_slot];
            const auto fid = s.fid.load(std::memory_order_acquire);
            if (!fid) return;
            AtkDump(a_slot, a_why);
            AtkPostDetach(fid, a_slot);
            AtkClearSlot(s);
        }

        // The worker tick (sleeper body, under PumpTickGate, after Scheduler::Tick).
        // Walks Followers::g_active + g_followers -- the worker's own domain (#4),
        // find() never operator[] (#9). "Managed" = tracked AND mfoEnabled.
        void AtkObserveTick() {
            if (!Config::g_attackObserve.load(std::memory_order_relaxed)) return;
            const auto nowMs = AtkNowMs();

            if (g_atkThreadState.load(std::memory_order_acquire) == 2) {
                spdlog::info("[atk-obs] sink thread={} main={} first-tag='{}'",
                             g_atkSinkThread, g_atkMainThread.load(std::memory_order_relaxed),
                             static_cast<const char*>(g_atkFirstTag));
                g_atkThreadState.store(3, std::memory_order_release);
            }
            int logged = 0;
            for (auto& e : g_atkUnk) {
                if (logged >= 4) break;   // rate limit: a few first-sight lines per wake
                if (e.state.load(std::memory_order_acquire) != 2) continue;
                spdlog::info("[atk-obs] new-tag '{}' on {:08X}", static_cast<const char*>(e.text),
                             e.firstFid.load(std::memory_order_relaxed));
                e.state.store(3, std::memory_order_release);
                ++logged;
            }

            bool seen[kAtkSlots] = {};
            for (const auto& h : Followers::g_active) {
                auto* actor = h.get().get();
                if (!actor) continue;
                const auto id  = actor->GetFormID();
                const auto rec = g_followers.find(id);
                if (rec == g_followers.end() || !rec->second.mfoEnabled) continue;
                std::size_t slot = kAtkSlots;
                for (std::size_t i = 0; i < kAtkSlots; ++i)
                    if (g_atk[i].fid.load(std::memory_order_acquire) == id) { slot = i; break; }
                if (slot == kAtkSlots) {
                    for (std::size_t i = 0; i < kAtkSlots; ++i)
                        if (g_atk[i].fid.load(std::memory_order_acquire) == 0 && !seen[i]) { slot = i; break; }
                    if (slot == kAtkSlots) {
                        if (!g_atkSlotsFullLogged.exchange(true))
                            spdlog::warn("[atk-obs] slot table full ({}) -- {:08X} '{}' is NOT observed",
                                         kAtkSlots, id, actor->GetName() ? actor->GetName() : "?");
                        continue;
                    }
                    auto& s = g_atk[slot];
                    AtkClearSlot(s);
                    AtkCopy(s.name, kAtkName, actor->GetName() ? actor->GetName() : "?", kAtkName - 1);
                    s.fid.store(id, std::memory_order_release);   // publish LAST: the sink's membership test
                }
                seen[slot] = true;
                AtkService(slot, actor, nowMs);
            }
            for (std::size_t i = 0; i < kAtkSlots; ++i)
                if (!seen[i]) AtkReleaseSlot(i, "dismissed/unmanaged");
        }

        // ONE persistent sleeper thread that only ever QUEUES to main -- the
        // family's established pattern ("sinks, sleeper threads, and menu
        // actions only queue"). Spawning a detached thread per tick would be
        // a thread-per-2s leak and a lifetime hazard on shutdown.
        //
        // This is the M3 stand-in for M5's real scheduler: deliberately dumb
        // and slow, because detection changes are all it needs to catch.
        void SleeperLoop(std::uint64_t a_epoch) {
            InstallTerminateHandlerOnThisThread();   // [fatal]: per-thread in MSVC; this thread is ours
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

                        // [fatal]: the terminate handler is per thread (MSVC) and
                        // this body runs on whichever job worker drains AddTask;
                        // one TLS read per tick, one install per worker thread.
                        thread_local bool t_terminateInstalled = false;
                        if (!t_terminateInstalled) {
                            t_terminateInstalled = true;
                            InstallTerminateHandlerOnThisThread();
                        }

                        g_hbWorkerTicks.fetch_add(1, std::memory_order_relaxed);   // [hb]

                        // Detection and the HUD stay on the old ~532 ms budget;
                        // only the evaluator runs at the deadline.
                        // [hb] HbStage names the step about to run, so a body that
                        // never returns leaves its callee's name in the heartbeat.
                        if (diagTurn) { HbStage("Followers::Refresh"); Followers::Refresh(); }

                        HbStage("Scheduler::Tick");               Scheduler::Tick();
                        HbStage("Actuation::SelfCastReconcile");  Actuation::SelfCastReconcile();     // release self-cast channels when their rule goes stale (dispel lingering buffs)
                        HbStage("Actuation::TargetCastReconcile"); Actuation::TargetCastReconcile();  // release on-target direct-force streams (heal/damage re-flow; dispel lingering ward)
                        HbStage("APMFBridge::Tick");              APMFBridge::Tick();                 // Phase 3: auto-release APMF owned-cast claims (spell+target) once the gambit stops firing (no-op without APMF)
                        HbStage("APMFBridge::MaybeWarnAbsence");  APMFBridge::MaybeWarnAbsence();     // gentle corner-toast reminder (~10 min cadence) when APMF is absent; no-op the instant it's present
                        HbStage("Loadout::Tick");                 Loadout::Tick();   // hand back stowed two-handers
                        HbStage("AtkObserveTick");                AtkObserveTick();  // [atk-obs] passive attack-event probe: slots, fights, idle-in-reach, dumps

                        if (diagTurn) { HbStage("Probe::Tick"); Probe::Tick(); }
                        // The board echoes edits back through the snapshot, so
                        // while it is OPEN publish every tick (133ms) instead of
                        // every 4th -- a half-second echo made DragFloat crawl
                        // and hid whether a click registered (board review M2).
                        if (diagTurn || Board::IsOpen()) { HbStage("Board::PublishSnapshot"); Board::PublishSnapshot(); }
                        HbStage("idle");
                    });
                }

                // [hb] the heartbeat line, from THIS thread (sleeps + AddTasks
                // only, so it survives a worker or main hang). Read the counts,
                // print, THEN post the next main probe: the line reports probes
                // that had run by the time it printed, so a stalled main shows
                // as a drains count that stops moving while ticks (or this line)
                // keep going. Post is a documented no-op on VR: drains stay 0.
                if (Config::g_heartbeat.load(std::memory_order_relaxed) && (wake % (kHeartbeatMs / kPumpMs)) == 0) {
                    const auto nowMs   = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch()).count();
                    const auto lastMain = g_hbMainLastMs.load(std::memory_order_relaxed);
                    spdlog::info("[hb] worker ticks={} main drains={} mainAge={}ms lastWorkerStage='{}'",
                                 g_hbWorkerTicks.load(std::memory_order_relaxed),
                                 g_hbMainDrains.load(std::memory_order_relaxed),
                                 lastMain ? (nowMs - lastMain) : -1,
                                 g_hbWorkerStage.load(std::memory_order_relaxed));
                    MainThread::Post([]() {
                        g_hbMainDrains.fetch_add(1, std::memory_order_relaxed);
                        g_hbMainLastMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch()).count(),
                                             std::memory_order_relaxed);
                    });
                }
            }
        }

    }

    // ── Cross-TU pump gate, defined out-of-line ───────────────────────────────
    // Same TU as the anonymous-namespace pump atomics above (they have internal
    // linkage but are visible here), so these reach g_tickActive/g_pumpRunning/
    // g_pumpPaused/g_pumpEpoch while the header keeps them out of other TUs. The
    // behaviour is byte-identical to the former in-anon-namespace definitions;
    // only the linkage/visibility changed (the SEV-1 wave shares the guard).
    std::uint64_t CurrentPumpEpoch() {
        return g_pumpEpoch.load(std::memory_order_seq_cst);
    }
    PumpTickGate::PumpTickGate(std::uint64_t a_queuedEpoch) {
        g_tickActive.store(true, std::memory_order_seq_cst);
        m_ok = g_pumpRunning.load(std::memory_order_seq_cst) &&
               !g_pumpPaused.load(std::memory_order_seq_cst) &&
               g_pumpEpoch.load(std::memory_order_seq_cst) == a_queuedEpoch;
    }
    PumpTickGate::~PumpTickGate() { g_tickActive.store(false, std::memory_order_seq_cst); }

    // ── [atk-obs] public entry points (worker / serial-pump domain) ─────────
    void DumpAttackHistogram(RE::FormID a_actorID) {
        if (!a_actorID) return;
        for (std::size_t i = 0; i < kAtkSlots; ++i) {
            if (g_atk[i].fid.load(std::memory_order_acquire) == a_actorID) {
                AtkDump(i, "explicit");
                return;
            }
        }
    }
    void ResetAttackObserve() {
        for (auto& s : g_atk) AtkClearSlot(s);   // sinks stay registered; fid=0 makes every event a no-op
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

    // ── [fatal] PASSIVE FATAL HOOK (2026-09-21) ──────────────────────────────
    // One LAST-position vectored exception handler + a std::terminate handler,
    // whose whole job is to make the log tail survive the death of the process:
    // flush spdlog, print ONE `[fatal]` line naming the code, the faulting
    // address as module+rva (MFO / APMF / the game image; else the raw address)
    // and the thread, then hand the exception straight back
    // (EXCEPTION_CONTINUE_SEARCH). It never handles, never swallows, never
    // unwinds: CrashLogger (an unhandled-exception filter, which runs AFTER
    // every vectored handler declines) keeps ownership of the crash report.
    //
    // What a `[fatal]` line means: an ERROR-severity SEH exception (code
    // 0xC0xxxxxx: access violation, illegal instruction, stack overflow, int
    // divide, in-page error, heap corruption, ...) was RAISED on that thread at
    // that address. Vectored handlers see FIRST-CHANCE exceptions, before any
    // __try/__except frame, so a line is NOT by itself proof the process died --
    // a module that probes memory under SEH would print one and carry on. The
    // log tells them apart: a `[fatal]` that is the LAST line is the crash; one
    // followed by ordinary lines was handled by somebody. Codes with the
    // customer bit (0xE06D7363 = a C++ throw, .NET 0xE0434352, thread-naming
    // 0x406D1388) and every warning/informational code are ignored, so try/catch
    // traffic never prints. Capped at kFatalMaxLines per session.
    //
    // Inside the handler: the process is dying, so do as little as a line
    // needs. No allocation beyond spdlog's own formatting, no engine call, no
    // lock but the sink's. A fault INSIDE the logger on the same thread would
    // re-enter here -- the thread_local guard bails, and the original exception
    // continues to CrashLogger. Stack overflow (0xC00000FD) has no headroom
    // for formatting; it gets the flush only. A fault raised while ANOTHER
    // thread holds the sink mutex waits on that mutex, like CrashLogger's own
    // handler would -- accepted.
    //
    // std::terminate (an uncaught C++ exception, a noexcept violation) does
    // not raise an SEH exception the vectored handler can see: MSVC's abort()
    // fast-fails past exception dispatch. So a terminate handler prints the
    // same line and then abort()s exactly as the default would. MSVC keeps the
    // terminate handler PER THREAD (set_terminate stores into the calling
    // thread's CRT data), so it is installed on each thread MFO owns or runs
    // on: the plugin-load thread here, the sleeper thread at its start, the
    // AddTask worker once per thread from the tick body (thread_local latch),
    // and the main thread through one MainThread::Post at StartPump. The
    // combat-thread hooks (Targeting / CasterConsent / CombatStyle) are not
    // covered -- adding a set_terminate there is a change to those files.
    //
    // <windows.h> is banned outside the vendored imgui backend (GetObject
    // hijack, ENGINE_NOTES §9), so the four Win32 shapes below are declared by
    // hand, exactly like GetModuleHandleA/GetCurrentThreadId at the top of this
    // file. Layouts are the x64 SDK ones: EXCEPTION_RECORD {DWORD code; DWORD
    // flags; EXCEPTION_RECORD* next; PVOID address; DWORD nparams; ULONG_PTR
    // info[15]} (0x98 bytes), EXCEPTION_POINTERS {EXCEPTION_RECORD*; CONTEXT*}.
    // PVECTORED_EXCEPTION_HANDLER returns LONG; NTAPI is __stdcall, a no-op
    // on x64. EXCEPTION_CONTINUE_SEARCH is 0.
    namespace {
        struct FatalExceptionRecord {
            unsigned long         code;
            unsigned long         flags;
            FatalExceptionRecord* next;
            void*                 address;
            unsigned long         numberParameters;
            std::uintptr_t        information[15];
        };
        static_assert(sizeof(FatalExceptionRecord) == 0x98, "EXCEPTION_RECORD x64 layout");
        struct FatalExceptionPointers {
            FatalExceptionRecord* record;
            void*                 context;
        };
        // A known image: [base, base+size). Size comes from the PE header the
        // loader mapped (IMAGE_NT_HEADERS64::OptionalHeader.SizeOfImage at
        // NT+0x50: Signature 4 + FileHeader 20 + 56 into the optional header).
        struct FatalModule {
            const char*                 name = nullptr;
            std::atomic<std::uintptr_t> base{ 0 };
            std::atomic<std::uintptr_t> size{ 0 };
        };
        FatalModule g_fatalModules[3] = { { "MFO.dll" }, { "APMF.dll" }, { "SkyrimSE.exe" } };

        std::atomic<int>  g_fatalLines{ 0 };
        constexpr int     kFatalMaxLines = 16;
        thread_local bool t_inFatalHandler = false;

        std::size_t PeImageSize(std::uintptr_t a_base) noexcept {
            if (!a_base) return 0;
            const auto* b = reinterpret_cast<const unsigned char*>(a_base);
            if (b[0] != 'M' || b[1] != 'Z') return 0;
            const auto  lfanew = *reinterpret_cast<const std::int32_t*>(b + 0x3C);
            const auto* nt     = b + lfanew;
            if (nt[0] != 'P' || nt[1] != 'E') return 0;
            return *reinterpret_cast<const std::uint32_t*>(nt + 0x50);
        }
        void FatalNoteModule(FatalModule& a_m, std::uintptr_t a_base) noexcept {
            if (!a_base) return;
            a_m.size.store(PeImageSize(a_base), std::memory_order_relaxed);
            a_m.base.store(a_base, std::memory_order_release);
        }
        // Resolve "module+rva" for the line. Falls back to the raw address.
        void FatalDescribe(std::uintptr_t a_addr, char* a_out, std::size_t a_cap) noexcept {
            for (auto& m : g_fatalModules) {
                const auto base = m.base.load(std::memory_order_acquire);
                const auto size = m.size.load(std::memory_order_relaxed);
                if (base && a_addr >= base && a_addr < base + size) {
                    std::snprintf(a_out, a_cap, "%s+0x%llX", m.name,
                                  static_cast<unsigned long long>(a_addr - base));
                    return;
                }
            }
            std::snprintf(a_out, a_cap, "0x%llX (no known module)", static_cast<unsigned long long>(a_addr));
        }

        long __stdcall FatalVectoredHandler(void* a_pointers) {
            constexpr long kContinueSearch = 0;
            auto* a_ep = static_cast<FatalExceptionPointers*>(a_pointers);
            if (!a_ep || !a_ep->record) return kContinueSearch;
            const auto code = a_ep->record->code;
            // ERROR severity (bits 31+30) without the customer bit (29): 0xC0xxxxxx.
            if ((code & 0xE0000000u) != 0xC0000000u) return kContinueSearch;
            if (t_inFatalHandler) return kContinueSearch;           // a fault inside this handler / the logger
            t_inFatalHandler = true;
            if (auto* log = spdlog::default_logger_raw()) {
                if (code == 0xC00000FDu) {                           // STACK_OVERFLOW: no room to format
                    log->flush();
                } else if (g_fatalLines.fetch_add(1, std::memory_order_relaxed) < kFatalMaxLines) {
                    char where[96];
                    FatalDescribe(reinterpret_cast<std::uintptr_t>(a_ep->record->address), where, sizeof where);
                    // critical -> flush_on fires; the flush() after is the belt for a sink
                    // whose flush level was raised.
                    log->critical("[fatal] code=0x{:08X} addr={} thread={} -- continuing search (CrashLogger owns the report)",
                                  code, static_cast<const char*>(where), ::GetCurrentThreadId());
                    log->flush();
                }
            }
            t_inFatalHandler = false;
            return kContinueSearch;
        }

        [[noreturn]] void FatalTerminateHandler() {
            if (!t_inFatalHandler) {
                t_inFatalHandler = true;
                if (auto* log = spdlog::default_logger_raw()) {
                    if (g_fatalLines.fetch_add(1, std::memory_order_relaxed) < kFatalMaxLines)
                        log->critical("[fatal] std::terminate thread={} -- aborting (uncaught C++ exception or noexcept violation)",
                                      ::GetCurrentThreadId());
                    log->flush();
                }
            }
            std::abort();
        }
    }
    void InstallTerminateHandlerOnThisThread() {
        std::set_terminate(FatalTerminateHandler);
    }

    void RefreshFatalModules() {
        // Our own image by ADDRESS (no dependence on the file name), the game
        // image from REL, APMF by name (absent -> stays 0 and never matches).
        // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS (0x4) | UNCHANGED_REFCOUNT (0x2).
        void* self = nullptr;
        if (::GetModuleHandleExA(0x4u | 0x2u, reinterpret_cast<const char*>(&FatalVectoredHandler), &self) && self)
            FatalNoteModule(g_fatalModules[0], reinterpret_cast<std::uintptr_t>(self));
        FatalNoteModule(g_fatalModules[1], reinterpret_cast<std::uintptr_t>(::GetModuleHandleA("APMF.dll")));
        g_fatalModules[2].name = REL::Module::IsVR() ? "SkyrimVR.exe" : "SkyrimSE.exe";   // label only; written before base publishes
        FatalNoteModule(g_fatalModules[2], REL::Module::get().base());
    }

    void InstallFatalHook() {
        static std::atomic<bool> installed{ false };
        if (installed.exchange(true)) return;
        RefreshFatalModules();
        InstallTerminateHandlerOnThisThread();
        void* h = ::AddVectoredExceptionHandler(0 /* last: after every other vectored handler */, &FatalVectoredHandler);
        spdlog::info("[fatal] passive hook {} (vectored handler LAST, std::terminate on this thread; MFO.dll @0x{:X} size 0x{:X}, game @0x{:X} size 0x{:X}, APMF {})",
                     h ? "installed" : "NOT installed (AddVectoredExceptionHandler returned null)",
                     g_fatalModules[0].base.load(), g_fatalModules[0].size.load(),
                     g_fatalModules[2].base.load(), g_fatalModules[2].size.load(),
                     g_fatalModules[1].base.load() ? "resolved" : "absent (re-resolved at kDataLoaded)");
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
            // [atk-obs] which attack frameworks are in this process (SKSE has loaded
            // every plugin DLL by kDataLoaded). Read-only, once.
            const auto yn = [](const char* a_dll) { return ::GetModuleHandleA(a_dll) ? "y" : "n"; };
            spdlog::info("[atk-obs] frameworks SCAR.dll={} BFCO.dll={} PayloadInterpreter.dll={} FollowerParkour.dll={} DualWieldParryingNG.dll={} | probe {} (bAttackObserve)",
                         yn("SCAR.dll"), yn("BFCO.dll"), yn("PayloadInterpreter.dll"), yn("FollowerParkour.dll"),
                         yn("DualWieldParryingNG.dll"), Config::g_attackObserve.load() ? "ON" : "off");
            for (std::size_t i = 0; i < kAtkSlots; ++i) g_atkSinks[i].slot = i;
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
        // [fatal]: the main thread's copy of the terminate handler, once. Post is
        // a documented no-op on VR (pump not installed) -- then main is uncovered.
        static std::atomic<bool> mainTerminateQueued{ false };
        if (!mainTerminateQueued.exchange(true))
            MainThread::Post([]() { InstallTerminateHandlerOnThisThread(); });
    }

}
