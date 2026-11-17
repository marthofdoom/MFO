#include "iaus/SwingSeq.h"
#include "MainThread.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <utility>

// iaus/SwingSeq.h is the contract; this file is the sink, the slot table and the main-thread attach.
// The sink / attach shape is Diagnostics.cpp's [atk-obs] (same graph, same pins), with a SEPARATE sink object so the
// probe's lifecycle (bAttackObserve, default OFF) is untouched.
namespace MFO::Iaus::SwingSeq {

    namespace {
        constexpr std::size_t   kSlots = 16;            // followers tracked at once (the same bound as [atk-obs])
        constexpr std::int64_t  kAttachMs = 2000;       // re-post the (deduped) attach this often: a graph is rebuilt on a 3D reload
        constexpr std::int64_t  kSameSwingMs = 150;     // a both-hands swing raises weaponSwing and weaponLeftSwing together
        constexpr std::int64_t  kIdleFreeMs = 60000;    // a slot not serviced this long is freed (dismissed follower)

        constexpr char Lo(char a_c) noexcept { return (a_c >= 'A' && a_c <= 'Z') ? static_cast<char>(a_c + 32) : a_c; }
        bool Eq(const char* a_a, const char* a_b) noexcept {
            for (;; ++a_a, ++a_b) {
                if (Lo(*a_a) != Lo(*a_b)) return false;
                if (!*a_a) return true;
            }
        }
        bool Prefix(const char* a_s, const char* a_p) noexcept {
            for (; *a_p; ++a_s, ++a_p)
                if (Lo(*a_s) != Lo(*a_p)) return false;
            return true;
        }

        enum class Tag : std::uint8_t { Other, Swing, Power, Bash };
        // The graph-raised tags of a swing, and the tags a power / bash MIGHT be told by. Only the swing keys the learner
        // (review F1): MCO_PowerWinOpen / BFCO_NextPowerWinStart are combo WINDOWS (a chance to chain into a power attack),
        // not a power attack, and bashStart / bashPowerStart / attackPowerStart* are behaviour INPUTS a graph sink never sees.
        // They are COUNTED so the `[iaus-fill] swing` line shows what each chain raised and a field run can prove one.
        Tag Classify(const char* a_tag) noexcept {
            if (Eq(a_tag, "weaponSwing") || Eq(a_tag, "weaponLeftSwing")) return Tag::Swing;
            if (Eq(a_tag, "MCO_PowerWinOpen") || Eq(a_tag, "BFCO_NextPowerWinStart") || Prefix(a_tag, "attackPowerStart")) return Tag::Power;
            if (Eq(a_tag, "bashStart") || Eq(a_tag, "bashPowerStart")) return Tag::Bash;
            return Tag::Other;
        }

        struct Slot {
            std::atomic<RE::FormID>     fid{ 0 };       // 0 = free; the sink's membership test
            std::atomic<std::uint32_t>  events{ 0 };    // every graph event seen (the attached proof)
            std::atomic<std::uint32_t>  swings{ 0 }, power{ 0 }, bash{ 0 };
            std::atomic<std::int64_t>   lastSwingMs{ 0 };
            // worker only
            std::int64_t lastAttachMs = 0, lastServiceMs = 0;
        };
        std::array<Slot, kSlots> g_slots;

        class Sink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
        public:
            explicit Sink(std::size_t a_slot) : slot(a_slot) {}   // set ONCE, before any graph can hold the sink (review F6)
            const std::size_t slot;
            RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_ev, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override {
                if (!a_ev || !a_ev->holder) return RE::BSEventNotifyControl::kContinue;
                Slot&      s = g_slots[slot];
                const auto fid = s.fid.load(std::memory_order_acquire);
                if (fid == 0 || a_ev->holder->GetFormID() != fid) return RE::BSEventNotifyControl::kContinue;
                const char* tag = a_ev->tag.c_str();
                if (!tag || !*tag) return RE::BSEventNotifyControl::kContinue;
                s.events.fetch_add(1, std::memory_order_relaxed);
                switch (Classify(tag)) {
                case Tag::Swing: {
                    const std::int64_t now = NowMs();
                    const std::int64_t was = s.lastSwingMs.exchange(now, std::memory_order_relaxed);
                    if (was == 0 || now - was >= kSameSwingMs) s.swings.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                case Tag::Power: s.power.fetch_add(1, std::memory_order_relaxed); break;
                case Tag::Bash:  s.bash.fetch_add(1, std::memory_order_relaxed); break;
                default: break;
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        template <std::size_t... I>
        std::array<Sink, kSlots> MakeSinks(std::index_sequence<I...>) { return { { Sink(I)... } }; }
        std::array<Sink, kSlots> g_sinks = MakeSinks(std::make_index_sequence<kSlots>{});

        // MAIN thread: attach this slot's sink to every graph of the actor (AddEventSink dedupes under its spin lock).
        void PostAttach(RE::FormID a_fid, std::size_t a_slot) {
            if (!MainThread::IsInstalled()) return;   // VR: no pump, so no sequence (the swing learner falls back to the attack state)
            MainThread::Post([a_fid, a_slot]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_fid);
                if (!actor || g_slots[a_slot].fid.load(std::memory_order_acquire) != a_fid) return;   // gone, or the slot moved on
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
                if (!actor->GetAnimationGraphManager(mgr) || !mgr) return;
                for (auto& g : mgr->graphs) {
                    if (!g) continue;
                    // BShkbAnimationGraph carries TWO BSTEventSource bases; name the one we mean (as [atk-obs] does).
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
        void ZeroCounters(Slot& a_s) {
            a_s.events.store(0, std::memory_order_relaxed);
            a_s.swings.store(0, std::memory_order_relaxed);
            a_s.power.store(0, std::memory_order_relaxed);
            a_s.bash.store(0, std::memory_order_relaxed);
            a_s.lastSwingMs.store(0, std::memory_order_relaxed);
        }
    }

    std::int64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void Service(RE::Actor* a_actor) {
        if (!a_actor) return;
        const RE::FormID fid = a_actor->GetFormID();
        const std::int64_t now = NowMs();
        std::size_t slot = kSlots, freeSlot = kSlots, stale = kSlots;
        for (std::size_t i = 0; i < kSlots; ++i) {
            const RE::FormID f = g_slots[i].fid.load(std::memory_order_acquire);
            if (f == fid) { slot = i; break; }
            if (f == 0) { if (freeSlot == kSlots) freeSlot = i; }
            else if (now - g_slots[i].lastServiceMs > kIdleFreeMs) stale = i;
        }
        if (slot == kSlots) {
            if (freeSlot == kSlots && stale != kSlots) {   // free a dismissed follower's slot: detach his sink, then reuse it
                const RE::FormID old = g_slots[stale].fid.load(std::memory_order_acquire);
                g_slots[stale].fid.store(0, std::memory_order_release);
                PostDetach(old, stale);
                freeSlot = stale;
            }
            if (freeSlot == kSlots) return;   // table full: he falls back to the attack-state swing (the learner says so)
            Slot& s = g_slots[freeSlot];
            ZeroCounters(s);
            s.lastAttachMs = 0;
            s.fid.store(fid, std::memory_order_release);   // publish LAST: the sink's membership test
            slot = freeSlot;
        }
        Slot& s = g_slots[slot];
        s.lastServiceMs = now;
        if (a_actor->Is3DLoaded() && now - s.lastAttachMs >= kAttachMs) {
            s.lastAttachMs = now;
            PostAttach(fid, slot);
        }
    }

    Obs Read(RE::FormID a_fid) {
        Obs o;
        if (a_fid == 0) return o;
        for (const Slot& s : g_slots) {
            if (s.fid.load(std::memory_order_acquire) != a_fid) continue;
            o.live        = s.events.load(std::memory_order_relaxed) > 0;
            o.swings      = s.swings.load(std::memory_order_relaxed);
            o.power       = s.power.load(std::memory_order_relaxed);
            o.bash        = s.bash.load(std::memory_order_relaxed);
            o.lastSwingMs = s.lastSwingMs.load(std::memory_order_relaxed);
            break;
        }
        return o;
    }

    void Clear() {
        for (Slot& s : g_slots) {
            s.fid.store(0, std::memory_order_release);
            ZeroCounters(s);
            s.lastAttachMs = s.lastServiceMs = 0;
        }
    }

}
