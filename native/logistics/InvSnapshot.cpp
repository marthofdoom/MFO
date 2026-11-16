#include "PCH.h"
#include "InvSnapshot.h"
#include "MainThread.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// logistics/InvSnapshot.cpp -- see InvSnapshot.h for the contract. INVARIANTS #84.
namespace MFO::InvSnap {

    namespace {

        // ── cadence (principle 9: every number here is counted in WORKER TICKS, 133 ms each, the unit the refresh really runs in) ──
        // The worker posts one build pass per tick and the main thread drains it on its next frame, so a snapshot read in tick k was
        // normally built between tick k-1 and k: age 1.
        // (The party and the player have no age cap of their own: their rule is TickClock::needPass, the previous tick's pass.)
        constexpr std::uint64_t kSourceRefreshTicks  = 4;    // an untouched container / corpse is re-read every ~0.5 s while wanted
        constexpr std::uint64_t kSourceMaxAgeTicks   = 16;   // ~2 s = refresh x 4: three missed refreshes (a pass builds at most kSourceBudget sources)
        // A want outlives the asker's own cadence: a follower is serviced once per LAP (party size x 133 ms) and a loot scan asks about
        // a source once per lap at best, so the floor is many laps, not one. 15 s covers a 20-follower party five times over.
        constexpr std::uint64_t kWantTtlTicks        = 112;  // ~15 s since the last ask, then the slot is forgotten
        constexpr std::size_t   kSourceBudget        = 32;   // sources built per pass (the party is never budgeted); the rest wait one pass

        struct Slot {
            Ptr            snap;                 // immutable once published
            std::uintptr_t refToken = 0;         // the asker's ref ADDRESS (identity only): the main build must resolve the same object
            bool           standing = false;     // Want()ed (the party and the player, every tick): rebuilt every pass, the short age cap.
                                                 // Anything else is a SOURCE (a container, a corpse, another actor): refreshed and budgeted.
            std::uint64_t  wantedTick = 0;       // the last tick it was asked for
            std::uint64_t  builtTick = 0;        // the worker tick current when the published snapshot was built
            std::uint64_t  builtAge = 0;         // the age clock (unpaused ticks) at that moment
            std::uint64_t  builtPass = 0;        // the post tick of the BuildPass that published it (RebuildOnMain does not move it)
            bool           everByPass = false;   // a BuildPass has published it at least once
            std::uint32_t  touchSeq = 0;         // bumped by Touch (a write MFO made)
            std::uint32_t  builtSeq = 0;         // the touchSeq captured BEFORE the published snapshot's walk began
            std::uint64_t  touchTick = 0;        // the tick of the last Touch: the snapshot stays readable for the rest of that tick
            bool           init = false;         // some asker wants the initialising walk (the default). False = every ask so far was a_noInit
            bool           builtInit = false;    // how the published snapshot was walked
        };

        std::mutex                           g_mx;      // LEAF: slot map only. Never held across an engine call.
        std::unordered_map<RE::FormID, Slot> g_slots;
        std::atomic<std::uint64_t>           g_tick{ 0 };
        std::atomic<std::uint64_t>           g_age{ 0 };          // unpaused ticks (BeginTick)
        std::atomic<std::uint64_t>           g_lastPaused{ 0 };   // the last tick that began paused
        std::atomic<bool>                    g_paused{ false };   // this tick began paused
        std::atomic<std::uint64_t>           g_needPass{ 0 };     // TickClock::needPass

        // `[invsnap]` counters (relaxed; a session count)
        std::atomic<std::uint64_t> g_asks{ 0 }, g_hits{ 0 }, g_liveReads{ 0 };
        std::atomic<std::uint64_t> g_missNever{ 0 }, g_missTouched{ 0 }, g_missOld{ 0 };
        std::atomic<std::uint64_t> g_builds{ 0 }, g_buildFail{ 0 }, g_budgetDeferred{ 0 }, g_passes{ 0 };
        std::atomic<std::uint64_t> g_worstAge{ 0 };

        Copy CopyOf(RE::ExtraDataList* a_xl) {
            Copy c;
            c.token = reinterpret_cast<std::uintptr_t>(a_xl);
            if (auto* x = a_xl->GetByType<RE::ExtraCount>(); x && x->count > 0) c.n = static_cast<std::int32_t>(x->count);
            c.worn     = a_xl->HasType<RE::ExtraWorn>();
            c.wornLeft = a_xl->HasType<RE::ExtraWornLeft>();
            if (auto* x = a_xl->GetByType<RE::ExtraOwnership>()) { c.hasOwnership = true; c.owner = x->owner; }
            if (auto* x = a_xl->GetByType<RE::ExtraUniqueID>()) { c.hasUid = true; c.uid = x->uniqueID; c.uidBase = x->baseID; }
            if (auto* x = a_xl->GetByType<RE::ExtraCharge>()) { c.hasCharge = true; c.charge = x->charge; }
            if (auto* x = a_xl->GetByType<RE::ExtraEnchantment>()) { c.hasEnch = true; c.ench = x->enchantment; c.enchCharge = x->charge; }
            c.cannotWear = a_xl->HasType(RE::ExtraDataType::kCannotWear);
            c.questAlias = a_xl->HasQuestObjectAlias();
            if (auto* x = a_xl->GetByType<RE::ExtraHealth>()) { c.hasHealth = true; c.health = x->health; }
            if (auto* x = a_xl->GetByType<RE::ExtraTextDisplayData>()) {
                c.hasText          = true;
                c.displayName      = x->displayName.c_str() ? x->displayName.c_str() : "";
                c.customNameLength = x->customNameLength;
                c.playerSetName    = x->IsPlayerSet();
            }
            return c;
        }

        // One engine entry into its value. a_entry is the LIVE entry (main thread), or a temporary for a base-container-only object.
        std::unique_ptr<Entry> EntryOf(RE::InventoryEntryData* a_entry) {
            auto e = std::make_unique<Entry>();
            e->object      = a_entry->object;
            e->worn        = a_entry->IsWorn();
            e->enchanted   = a_entry->IsEnchanted();
            e->value       = a_entry->GetValue();
            if (a_entry->extraLists) {
                e->hasLists = true;
                for (auto* xl : *a_entry->extraLists)
                    if (xl) e->copies.push_back(CopyOf(xl));
            }
            // InventoryEntryData::IsQuestObject(): any list with a quest-object alias (the one engine call per list, made in CopyOf).
            for (const auto& c : e->copies) if (c.questAlias) { e->questObject = true; break; }
            return e;
        }

        bool IsLeveledEntry(const RE::InventoryEntryData* a_entry) {
            if (!a_entry->extraLists) return false;
            for (auto* xl : *a_entry->extraLists)
                if (xl && xl->HasType<RE::ExtraLeveledItem>()) return true;
            return false;
        }

    }

    // THE WALK. The same two passes, in the same order, with the same rules as RE::TESObjectREFR::GetInventory (mit-3.7 fork
    // src/RE/T/TESObjectREFR.cpp:331): every changes entry by its countDelta (first entry of an object wins), then every base
    // container object unless its changes entry is leveled, added to the delta or inserted with a plain entry.
    Ptr BuildLive(RE::TESObjectREFR* a_ref, bool a_noInit) {
        if (!a_ref) return nullptr;
        auto snap   = std::make_shared<Snap>();
        snap->ref   = a_ref->GetFormID();
        std::unordered_set<const RE::TESBoundObject*> leveled;
        if (auto* changes = a_ref->GetInventoryChanges(a_noInit); changes && changes->entryList) {
            for (auto* entry : *changes->entryList) {
                if (!entry || !entry->object) continue;
                if (snap->items.contains(entry->object)) continue;
                if (IsLeveledEntry(entry)) leveled.insert(entry->object);
                snap->items.emplace(entry->object, std::make_pair(entry->countDelta, EntryOf(entry)));
            }
        }
        if (auto* container = a_ref->GetContainer()) {
            container->ForEachContainerObject([&](RE::ContainerObject& a_entry) {
                auto* obj = a_entry.obj;
                if (obj && !leveled.contains(obj)) {
                    if (auto it = snap->items.find(obj); it != snap->items.end()) {
                        it->second.first += a_entry.count;
                    } else {
                        RE::InventoryEntryData plain(obj, 0);
                        snap->items.emplace(obj, std::make_pair(a_entry.count, EntryOf(&plain)));
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        return snap;
    }

    const Entry* Snap::Find(const RE::TESBoundObject* a_obj) const {
        const auto it = items.find(const_cast<RE::TESBoundObject*>(a_obj));
        return it == items.end() ? nullptr : it->second.second.get();
    }

    std::int32_t Snap::Count(const RE::TESBoundObject* a_obj) const {
        const auto it = items.find(const_cast<RE::TESBoundObject*>(a_obj));
        return it == items.end() ? 0 : it->second.first;
    }

    const ItemMap& View::map() const& noexcept {
        static const ItemMap kNone;
        return _snap ? _snap->items : kNone;
    }

    View Items(RE::TESObjectREFR* a_ref, bool a_noInit) { return View(Get(a_ref, a_noInit)); }

    RE::TESObjectARMO* Snap::WornArmor(RE::BGSBipedObjectForm::BipedObjectSlot a_slot) const {
        for (const auto& [item, data] : items) {
            if (!item || !item->IsArmor()) continue;
            if (data.first > 0 && data.second->worn) {
                auto* armor = item->As<RE::TESObjectARMO>();
                if (armor && armor->HasPartOf(a_slot)) return armor;
            }
        }
        return nullptr;
    }

    RE::TESObjectARMO* Snap::WornArmor(RE::FormID a_formID) const {
        for (const auto& [item, data] : items) {
            if (!item || !item->IsArmor() || item->GetFormID() != a_formID) continue;
            if (data.first > 0 && data.second->worn) return item->As<RE::TESObjectARMO>();
        }
        return nullptr;
    }

    namespace {

        enum class Miss { None, Never, Touched, Old };

        // Under g_mx. Is a_s readable in the tick a_clk describes? Every input changes only at a tick boundary (BeginTick) or by a
        // publish that keeps it readable, so the answer for one ref does not flip from yes to no inside a tick (the tick gate
        // relies on that); the one exception is the init flip just below, which only a SOURCE can meet.
        Miss Readable(const Slot& a_s, const TickClock& a_clk) {
            const std::uint64_t a_now = a_clk.tick;
            if (!a_s.snap) return Miss::Never;
            if (a_s.init && !a_s.builtInit) return Miss::Never;   // built the no-init way, now asked the initialising way: not that answer
            // A write MFO made since the walk: readable only for the rest of the tick that made it (tick-stable; the take road
            // re-validates at its own write), then withheld until a snapshot built after the write is published.
            if (a_s.builtSeq != a_s.touchSeq && a_s.touchTick != a_now) return Miss::Touched;
            if (a_s.standing) {   // the party's rule: the previous tick's pass has published it
                if (!PassReached(a_clk, a_s.everByPass, a_s.builtPass)) return Miss::Old;
            } else if (!FreshEnough(a_clk, a_s.builtTick, a_s.builtAge, kSourceMaxAgeTicks)) return Miss::Old;
            return Miss::None;
        }

        void NoteAge(std::uint64_t a_age) {
            auto worst = g_worstAge.load(std::memory_order_relaxed);
            while (a_age > worst && !g_worstAge.compare_exchange_weak(worst, a_age, std::memory_order_relaxed)) {}
        }

        // Under g_mx: the slot for a_ref, reset when the FormID now names a different object than the one it was asked for.
        Slot& SlotFor(RE::TESObjectREFR* a_ref, std::uint64_t a_now, bool a_noInit) {
            auto& s = g_slots[a_ref->GetFormID()];
            const auto token = reinterpret_cast<std::uintptr_t>(a_ref);
            if (s.refToken != token) {
                s = Slot{};
                s.refToken = token;
            }
            s.wantedTick = a_now;
            if (!a_noInit) s.init = true;
            return s;
        }

        Ptr Ask(RE::TESObjectREFR* a_ref, bool a_count, bool a_noInit) {
            const auto clk = Now();
            const auto now = clk.tick;
            Miss miss = Miss::None;
            Ptr  out;
            {
                std::scoped_lock lk(g_mx);
                auto& s = SlotFor(a_ref, now, a_noInit);
                miss = Readable(s, clk);
                if (miss == Miss::None) {
                    out = s.snap;
                    NoteAge(clk.age >= s.builtAge ? clk.age - s.builtAge : 0);
                }
            }
            if (a_count) {
                g_asks.fetch_add(1, std::memory_order_relaxed);
                switch (miss) {
                case Miss::None:    g_hits.fetch_add(1, std::memory_order_relaxed); break;
                case Miss::Never:   g_missNever.fetch_add(1, std::memory_order_relaxed); break;
                case Miss::Touched: g_missTouched.fetch_add(1, std::memory_order_relaxed); break;
                case Miss::Old:     g_missOld.fetch_add(1, std::memory_order_relaxed); break;
                }
            }
            return out;
        }

    }

    Ptr Get(RE::TESObjectREFR* a_ref, bool a_noInit) {
        if (!a_ref) return nullptr;
        if (MainThread::LiveReadsAllowed()) {
            g_liveReads.fetch_add(1, std::memory_order_relaxed);
            auto snap = std::const_pointer_cast<Snap>(BuildLive(a_ref, a_noInit));
            if (snap) snap->live = true;
            return snap;
        }
        return Ask(a_ref, true, a_noInit);
    }

    bool Ready(RE::TESObjectREFR* a_ref) {
        if (!a_ref) return false;
        if (MainThread::LiveReadsAllowed()) return true;
        return Ask(a_ref, false, false) != nullptr;
    }

    void Want(RE::TESObjectREFR* a_ref) {
        if (!a_ref || !MainThread::IsInstalled()) return;
        const auto now = g_tick.load(std::memory_order_acquire);
        std::scoped_lock lk(g_mx);
        SlotFor(a_ref, now, false).standing = true;
    }

    void Touch(RE::FormID a_ref) {
        if (!a_ref) return;
        const auto now = g_tick.load(std::memory_order_acquire);
        std::scoped_lock lk(g_mx);
        if (auto it = g_slots.find(a_ref); it != g_slots.end()) {
            ++it->second.touchSeq;
            it->second.touchTick = now;
        }
    }

    void Touch(RE::TESObjectREFR* a_ref) {
        if (a_ref) Touch(a_ref->GetFormID());
    }

    void RebuildOnMain(RE::TESObjectREFR* a_ref) {
        if (!a_ref) return;
        const auto id = a_ref->GetFormID();
        std::uint32_t seq = 0;
        bool          init = false;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_slots.find(id);
            if (it == g_slots.end() || it->second.refToken != reinterpret_cast<std::uintptr_t>(a_ref)) return;   // nobody reads it: nothing to replace
            seq  = it->second.touchSeq;
            init = it->second.init;
        }
        auto snap = BuildLive(a_ref, !init);
        g_builds.fetch_add(1, std::memory_order_relaxed);
        const auto clk = Now();
        std::scoped_lock lk(g_mx);
        const auto it = g_slots.find(id);
        if (it == g_slots.end() || it->second.refToken != reinterpret_cast<std::uintptr_t>(a_ref)) return;
        it->second.snap      = std::move(snap);
        it->second.builtSeq  = seq;
        it->second.builtTick = clk.tick;
        it->second.builtAge  = clk.age;
        it->second.builtInit = init;
    }

    void BeginTick(bool a_paused) {
        const auto t = g_tick.fetch_add(1, std::memory_order_acq_rel) + 1;
        g_paused.store(a_paused, std::memory_order_release);
        g_needPass.store(t - 1, std::memory_order_release);   // the pass the previous tick posted (read in unpaused ticks only)
        if (a_paused) g_lastPaused.store(t, std::memory_order_release);
        else          g_age.fetch_add(1, std::memory_order_acq_rel);
    }
    std::uint64_t CurrentTick() { return g_tick.load(std::memory_order_acquire); }
    TickClock Now() {
        TickClock c;
        c.tick       = g_tick.load(std::memory_order_acquire);
        c.age        = g_age.load(std::memory_order_acquire);
        c.lastPaused = g_lastPaused.load(std::memory_order_acquire);
        c.paused     = g_paused.load(std::memory_order_acquire);
        c.needPass   = g_needPass.load(std::memory_order_acquire);
        return c;
    }
    bool FreshEnough(const TickClock& a_now, std::uint64_t a_builtTick, std::uint64_t a_builtAge, std::uint64_t a_maxAge) {
        // A PAUSED tick: nothing can be rebuilt and nothing the engine owns moves, so the snapshot it has stands (no aging).
        if (a_now.paused) return true;
        // An UNPAUSED tick reads only what was built since the last paused tick began (a menu may have moved items) ...
        if (a_builtTick < a_now.lastPaused) return false;
        // ... and no older than the cap, counted in unpaused ticks.
        return (a_now.age >= a_builtAge ? a_now.age - a_builtAge : 0) <= a_maxAge;
    }

    // MAIN THREAD. One pass per worker tick: Diagnostics.cpp posts it at the tick's end, behind every main-thread write that tick
    // posted (the queue is FIFO), and drops a pass that a newer queued one supersedes (a stalled frame).
    void BuildPass(std::uint64_t a_postTick) {
        const auto clk = Now();
        const auto now = clk.tick;
        g_passes.fetch_add(1, std::memory_order_relaxed);

        struct Due { RE::FormID id; std::uintptr_t token; std::uint32_t seq; bool standing; std::uint64_t age; bool init; };
        std::vector<Due> due;
        {
            std::scoped_lock lk(g_mx);
            for (auto it = g_slots.begin(); it != g_slots.end();) {
                auto& s = it->second;
                if (now > s.wantedTick + kWantTtlTicks) { it = g_slots.erase(it); continue; }   // nobody asked for ~15 s
                const std::uint64_t age = s.snap ? (clk.age >= s.builtAge ? clk.age - s.builtAge : 0) : ~0ull;
                const bool touched = s.snap && s.builtSeq != s.touchSeq;
                const bool reinit = s.snap && ((s.init && !s.builtInit) || s.builtTick < clk.lastPaused);   // ... or built before the last pause
                if (!s.snap || touched || reinit || s.standing || age >= kSourceRefreshTicks)
                    due.push_back({ it->first, s.refToken, s.touchSeq, s.standing, (touched || reinit) ? ~0ull : age, s.init });
                ++it;
            }
        }
        // The party first (never budgeted), then sources: never built and touched ones (age ~0) first, then the oldest.
        std::stable_sort(due.begin(), due.end(), [](const Due& a, const Due& b) {
            if (a.standing != b.standing) return a.standing;
            return a.age > b.age;
        });
        std::size_t sources = 0;
        for (const auto& d : due) {
            if (!d.standing && ++sources > kSourceBudget) { g_budgetDeferred.fetch_add(1, std::memory_order_relaxed); continue; }
            auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(d.id);
            // The FormID must still name the object the worker asked about (a reused temporary id names another one).
            Ptr snap = (ref && reinterpret_cast<std::uintptr_t>(ref) == d.token) ? BuildLive(ref, !d.init) : nullptr;
            if (snap) g_builds.fetch_add(1, std::memory_order_relaxed);
            else      g_buildFail.fetch_add(1, std::memory_order_relaxed);
            std::scoped_lock lk(g_mx);
            const auto it = g_slots.find(d.id);
            if (it == g_slots.end() || it->second.refToken != d.token) continue;
            // Unresolved (the ref is gone, deleted, or the id names another object now): nothing is published. The old snapshot is
            // NOT dropped here, so a reader's "ready" answer never changes in the middle of a tick; it ages out at a tick boundary
            // (the party at the next tick, by its pass rule; a source at kSourceMaxAgeTicks) and reads "not yet" from then on.
            if (!snap) continue;
            it->second.snap      = std::move(snap);
            it->second.builtSeq  = d.seq;
            it->second.builtTick = clk.tick;
            it->second.builtAge  = clk.age;
            it->second.builtInit = d.init;
            it->second.builtPass = a_postTick;
            it->second.everByPass = true;
        }
    }

    void Clear() {
        std::scoped_lock lk(g_mx);
        g_slots.clear();
    }

    void LogStats() {
        std::size_t slots = 0;
        {
            std::scoped_lock lk(g_mx);
            slots = g_slots.size();
        }
        spdlog::info("[invsnap] since start: asks={} answered={} notYet(never built={} withheld after a write={} too old={}) liveReads(main)={} | "
                     "passes={} builds={} unresolved={} budgetDeferred={} slots={} worstAgeRead={} ticks",
                     g_asks.load(std::memory_order_relaxed), g_hits.load(std::memory_order_relaxed),
                     g_missNever.load(std::memory_order_relaxed), g_missTouched.load(std::memory_order_relaxed),
                     g_missOld.load(std::memory_order_relaxed), g_liveReads.load(std::memory_order_relaxed),
                     g_passes.load(std::memory_order_relaxed), g_builds.load(std::memory_order_relaxed),
                     g_buildFail.load(std::memory_order_relaxed), g_budgetDeferred.load(std::memory_order_relaxed), slots,
                     g_worstAge.load(std::memory_order_relaxed));
    }

}
