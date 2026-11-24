#include "probes/Probes_internal.h"

#include "MainThread.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <unordered_map>
#include <utility>

#include <spdlog/spdlog.h>

// [probe-trav]: the follower's traversal moves as his own animation graph reports them (8.8 item 6). iaus-sim-v4.md 8.8: the capability must
// be MEASURED per actor, so the line carries what the calibration script needs: the event, the heights at both ends, the time between, the
// player's place, what MFO had in flight, who else was mid-move, and whether a reader of 8.8 (3) saw trouble during it.
//
// THE SINK is the [atk-obs] / SwingSeq contract: it runs on whatever thread the graph dispatches on, so it touches fixed-size atomics ONLY
// (no map, no lock, no allocation, no log, no engine call but the holder's FormID). It matches the tag against kEvents and pushes
// {fid, kind, ms} into a fixed ring. The WORKER drains the ring each tick, and takes each end's position from the follower's own sample ring
// (Probes.cpp): so a start's position is the newest sample at or before the event, at most one sample period (~120 ms) off.
// The attach / detach are MainThread::Post'd (graph mutation is main-thread work), capturing a FormID and a slot only.
namespace MFO::Probes {

    namespace {
        enum class EvKind : std::uint8_t { Start = 1, Stop = 2, Interrupt = 3 };

        // THE ADAPTER TABLE (8.8 (1)): one row per traversal mod's graph event. Add a mod = add rows. SkyParkour family (Follower Parkour Plus
        // 2.8 on SkyParkour V, names read from its ini and DLL strings, NOT verified in the field: this probe is what verifies them).
        struct EvRow { const char* name; EvKind kind; };
        constexpr EvRow kEvents[] = {
            { "SkyParkour_Start",     EvKind::Start },
            { "SkyParkour_Stop",      EvKind::Stop },
            { "SkyParkour_Interrupt", EvKind::Interrupt },
        };
        // Graph variables read on the MAIN thread at each end and printed (presence + value: the "he runs a graph that knows such moves" source).
        constexpr const char* kVars[] = { "SkyParkourLedge", "SkyParkourOngoing", "SkyParkourSliding", "SkyParkourLowerBody" };

        constexpr char Lo(char a_c) noexcept { return (a_c >= 'A' && a_c <= 'Z') ? static_cast<char>(a_c + 32) : a_c; }
        bool Eq(const char* a_a, const char* a_b) noexcept {
            for (;; ++a_a, ++a_b) {
                if (Lo(*a_a) != Lo(*a_b)) return false;
                if (!*a_a) return true;
            }
        }

        // ---- the ring (sink writes, worker reads) -------------------------------------------------------------------------------------
        constexpr std::size_t kRingN = 64;
        struct Ev {
            std::atomic<std::uint64_t> seq{ 0 };   // n + 1 once written (published LAST)
            std::atomic<std::uint32_t> fid{ 0 };
            std::atomic<std::uint8_t>  kind{ 0 };
            std::atomic<std::uint8_t>  row{ 0 };
            std::atomic<std::int64_t>  ms{ 0 };
        };
        std::array<Ev, kRingN>      g_ring;
        std::atomic<std::uint64_t>  g_head{ 0 };
        std::uint64_t               g_tail = 0;   // worker

        constexpr std::size_t kSlots = 16;
        struct Slot {
            std::atomic<RE::FormID> fid{ 0 };   // 0 = free; the sink's membership test
            std::int64_t lastAttachMs = 0, lastServiceMs = 0;   // worker
        };
        std::array<Slot, kSlots> g_slots;

        class Sink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
        public:
            explicit Sink(std::size_t a_slot) : slot(a_slot) {}
            const std::size_t slot;
            RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_ev, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override {
                if (!a_ev || !a_ev->holder) return RE::BSEventNotifyControl::kContinue;
                const auto fid = g_slots[slot].fid.load(std::memory_order_acquire);
                if (fid == 0 || a_ev->holder->GetFormID() != fid) return RE::BSEventNotifyControl::kContinue;
                const char* tag = a_ev->tag.c_str();
                if (!tag || !*tag) return RE::BSEventNotifyControl::kContinue;
                for (std::size_t r = 0; r < std::size(kEvents); ++r) {
                    if (!Eq(tag, kEvents[r].name)) continue;
                    const auto n = g_head.fetch_add(1, std::memory_order_acq_rel);
                    Ev& e = g_ring[n % kRingN];
                    e.fid.store(fid, std::memory_order_relaxed);
                    e.kind.store(static_cast<std::uint8_t>(kEvents[r].kind), std::memory_order_relaxed);
                    e.row.store(static_cast<std::uint8_t>(r), std::memory_order_relaxed);
                    e.ms.store(NowMs(), std::memory_order_relaxed);
                    e.seq.store(n + 1, std::memory_order_release);
                    break;
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        template <std::size_t... I>
        std::array<Sink, kSlots> MakeSinks(std::index_sequence<I...>) { return { { Sink(I)... } }; }
        std::array<Sink, kSlots> g_sinks = MakeSinks(std::make_index_sequence<kSlots>{});

        void PostAttach(RE::FormID a_fid, std::size_t a_slot) {
            if (!MainThread::IsInstalled()) return;   // VR: no pump, so no probe
            MainThread::Post([a_fid, a_slot]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_fid);
                if (!actor || g_slots[a_slot].fid.load(std::memory_order_acquire) != a_fid) return;
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
                if (!actor->GetAnimationGraphManager(mgr) || !mgr) return;
                for (auto& g : mgr->graphs) {
                    if (!g) continue;
                    // BShkbAnimationGraph carries TWO BSTEventSource bases; name the one we mean (as [atk-obs] and SwingSeq do).
                    static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(g.get())->AddEventSink(&g_sinks[a_slot]);
                }
            });
        }
        void PostDetach(RE::FormID a_fid, std::size_t a_slot) {
            if (!MainThread::IsInstalled()) return;
            MainThread::Post([a_fid, a_slot]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_fid);
                if (!actor) return;
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
                if (!actor->GetAnimationGraphManager(mgr) || !mgr) return;
                for (auto& g : mgr->graphs)
                    if (g) static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(g.get())->RemoveEventSink(&g_sinks[a_slot]);
            });
        }

        // ---- pairing (worker) ---------------------------------------------------------------------------------------------------------
        struct Pend {
            std::int64_t  startMs = 0;
            Sample        start{};
            std::uint32_t livedAtStart = 0;
        };
        std::unordered_map<RE::FormID, Pend> g_pend;

        // Build one line on the worker, finish it (the graph variables) and log it on the MAIN thread.
        void Emit(RE::FormID a_fid, std::string a_line) {
            const auto finish = [a_fid, line = std::move(a_line)]() {
                std::string vars;
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(a_fid)) {
                    for (const char* v : kVars) {
                        bool b = false;
                        vars += fmt::format("{}={} ", v, a->GetGraphVariableBool(v, b) ? (b ? "1" : "0") : "absent");
                    }
                }
                spdlog::info("{} | graph vars: {}", line, vars);
            };
            if (MainThread::IsInstalled()) MainThread::Post(finish);
            else finish();   // no pump (VR): the graph variables are a main-thread read, so the line goes out without them
        }

        void Close(RE::FormID a_fid, const Pend& a_p, const char* a_how, const char* a_evName, std::int64_t a_endMs, const Sample* a_end,
                   bool a_haveStart) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_fid);
            auto* t = TrackOf(a_fid);
            const std::string inflight = InFlight(actor);
            const std::uint32_t livedNow = LivedCount(a_fid);
            const std::uint32_t lived = livedNow >= a_p.livedAtStart ? livedNow - a_p.livedAtStart : 0u;
            if (a_haveStart && inflight.find("leg") != std::string::npos)
                NoteLived(Lived::TraversalUnplanned, a_fid, fmt::format("{} while a loot leg was in flight", a_evName).c_str());
            if (!LineAllowed(Kind::Trav)) return;
            std::string player = "?";
            if (const auto* pc = RE::PlayerCharacter::GetSingleton()) { const auto p = pc->GetPosition(); player = PosWord(p.x, p.y, p.z); }
            const float dur = a_haveStart ? (a_endMs - a_p.startMs) / 1000.0f : -1.0f;
            std::string start = a_haveStart ? PosWord(a_p.start.x, a_p.start.y, a_p.start.z) : std::string("unknown (no start seen)");
            std::string end   = a_end ? PosWord(a_end->x, a_end->y, a_end->z) : std::string("?");
            const float dz    = (a_haveStart && a_end) ? a_end->z - a_p.start.z : 0.0f;
            std::size_t others = 0;
            for (const auto& [f, p] : g_pend) if (f != a_fid) ++others;
            Emit(a_fid, fmt::format("[probe-trav] {:08X} '{}': {} ({}) dur={:.2f}s start={} stop={} dz={:+.0f} | player {} | in flight: {} | others mid-move: {} | "
                                    "lived signs during the move: {}",
                                    a_fid, t ? t->name : std::string("?"), a_how, a_evName, dur, start, end, dz, player, inflight, others, lived));
        }
    }

    void TraversalService(RE::Actor* a_actor) {
        if (!a_actor) return;
        const RE::FormID fid = a_actor->GetFormID();
        const auto now = NowMs();
        std::size_t slot = kSlots, freeSlot = kSlots, stale = kSlots;
        for (std::size_t i = 0; i < kSlots; ++i) {
            const RE::FormID f = g_slots[i].fid.load(std::memory_order_acquire);
            if (f == fid) { slot = i; break; }
            if (f == 0) { if (freeSlot == kSlots) freeSlot = i; }
            else if (now - g_slots[i].lastServiceMs > kTuning.idleFreeMs) stale = i;
        }
        if (slot == kSlots) {
            if (freeSlot == kSlots && stale != kSlots) {
                const RE::FormID old = g_slots[stale].fid.load(std::memory_order_acquire);
                g_slots[stale].fid.store(0, std::memory_order_release);
                PostDetach(old, stale);
                freeSlot = stale;
            }
            if (freeSlot == kSlots) return;   // table full: he is not observed
            g_slots[freeSlot].lastAttachMs = 0;
            g_slots[freeSlot].fid.store(fid, std::memory_order_release);   // publish LAST: the sink's membership test
            slot = freeSlot;
        }
        Slot& s = g_slots[slot];
        s.lastServiceMs = now;
        if (a_actor->Is3DLoaded() && now - s.lastAttachMs >= kTuning.attachMs) {
            s.lastAttachMs = now;
            PostAttach(fid, slot);
        }
    }

    void TraversalDrain(std::int64_t a_nowMs) {
        const auto head = g_head.load(std::memory_order_acquire);
        if (head - g_tail > kRingN) g_tail = head - kRingN;   // the worker fell behind: the oldest were overwritten
        for (; g_tail < head; ++g_tail) {
            Ev& e = g_ring[g_tail % kRingN];
            if (e.seq.load(std::memory_order_acquire) != g_tail + 1) break;   // not published yet: next tick
            const RE::FormID fid = e.fid.load(std::memory_order_relaxed);
            const auto kind = static_cast<EvKind>(e.kind.load(std::memory_order_relaxed));
            const auto row = e.row.load(std::memory_order_relaxed);
            const auto ms = e.ms.load(std::memory_order_relaxed);
            if (!Enabled(Kind::Trav)) continue;
            Track* t = TrackOf(fid);
            if (t) t->lastEventMs = ms;
            const Sample* at = t ? AtOrBefore(*t, ms) : nullptr;
            const char* name = row < std::size(kEvents) ? kEvents[row].name : "?";
            if (kind == EvKind::Start) {
                Pend p;
                p.startMs = ms;
                if (at) p.start = *at;
                p.livedAtStart = LivedCount(fid);
                if (const auto it = g_pend.find(fid); it != g_pend.end())
                    Close(fid, it->second, "NO-STOP (a new start came first)", name, ms, at, true);
                g_pend[fid] = p;
                continue;
            }
            const auto it = g_pend.find(fid);
            if (it == g_pend.end()) { Close(fid, Pend{}, kind == EvKind::Stop ? "STOP" : "INTERRUPT", name, ms, at, false); continue; }
            Close(fid, it->second, kind == EvKind::Stop ? "STOP" : "INTERRUPT", name, ms, at, true);
            g_pend.erase(it);
        }
        if (g_pend.empty()) return;
        for (auto it = g_pend.begin(); it != g_pend.end();) {
            if (a_nowMs - it->second.startMs <= kTuning.moveWatchdogMs) { ++it; continue; }
            Track* t = TrackOf(it->first);
            Close(it->first, it->second, "NO-STOP (watchdog)", "none", a_nowMs, t ? Newest(*t) : nullptr, true);
            it = g_pend.erase(it);
        }
    }

    void TraversalClear() {
        g_pend.clear();
        g_tail = g_head.load(std::memory_order_acquire);
        for (auto& s : g_slots) s.fid.store(0, std::memory_order_release);   // the sinks stay on live graphs and now fail the membership test
    }

}
