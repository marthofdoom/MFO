#include "PCH.h"
#include "Runtime.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
#include "MainThread.h"

// Hand-declared like Diagnostics.cpp's (CommonLib never includes windows.h). user32: the owner thread of a window.
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId();
extern "C" __declspec(dllimport) unsigned long __stdcall GetWindowThreadProcessId(void* a_wnd, unsigned long* a_pid);

namespace MFO::MainThread {

    namespace {

        // The queue crosses threads BY DESIGN -- workers Post, the player's
        // Update drains -- so it gets a real lock (INVARIANTS #4). Contention
        // is a handful of Posts per second against one swap per frame; a plain
        // mutex is plenty, and swap-then-run below keeps the critical section
        // to a pointer exchange.
        std::mutex g_mx;
        std::vector<std::function<void()>> g_queue;

        std::atomic<bool> g_installed{ false };

        // Set when Install() REFUSES (VR). Post() then drops fns instead of
        // queueing them forever into a queue nothing will ever drain.
        std::atomic<bool> g_dead{ false };
        std::atomic<std::uint32_t> g_clearGen{ 0 };   // ClearGeneration()

        // ── WHICH THREAD IS THIS (INVARIANTS #84) ──
        // t_inSeat: this thread is inside the thunk below right now (the engine's player Update, then the drain).
        // g_mainTid: the main thread's OS id, 0 = not known yet. SEEDED ONCE from the ENGINE'S OWN RECORD (SeedMainThread, called
        // by Install): Main's constructor stores GetCurrentThreadId() at Main +0x28 on the WinMain thread, the thread that then
        // runs the frame loop (Main::Update). It is NOT "the thread this DLL was called on": SKSE sends kDataLoaded from the
        // engine's InitTESThread (INVARIANTS #84 rule 8, ENGINE_NOTES 0.55), which is how the first kPostLoadGame of a session
        // used to be judged off-main. The thunk only CHECKS it from then on (NoteSeatThread says so loudly if a frame differs).
        // t_workerTick: this thread is inside MFO's AddTask tick body (WorkerTickScope). The job system may hand that task to ANY
        // thread, so a tick that happens to run on the main thread's id is still a worker tick for every reader.
        thread_local bool              t_inSeat = false;
        thread_local int               t_workerTick = 0;
        std::atomic<std::uint32_t>     g_mainTid{ 0 };
        static_assert(offsetof(RE::Main, wnd) == 0x18 && offsetof(RE::Main, threadID) == 0x28,
                      "Main::wnd / Main::threadID: verified at +0x18 / +0x28 in Main's constructor on 1.5.97, 1.6.1170 and 1.7.104");

        // THE SEAT'S THREAD IS NOT THE RECORDED ONE (cold: at most once per frame, and only while they differ). The thunk runs
        // inside the engine's player Update, so its thread IS the frame thread by observation: it is adopted, and the
        // difference is SAID (principle 7: every main-thread test since the seeding answered for the other id).
        void NoteSeatThread(std::uint32_t a_recorded, std::uint32_t a_seat) noexcept {
            g_mainTid.store(a_seat, std::memory_order_relaxed);
            static std::atomic<int> s_said{ 0 };
            if (s_said.fetch_add(1, std::memory_order_relaxed) >= 4) return;
            try {
                if (a_recorded == 0)
                    spdlog::warn("[mainthread] the player Update runs on thread {}; no main thread was recorded before it (the "
                                 "engine's record was refused or not read, see the line at kDataLoaded). Taken as the main thread "
                                 "from this frame on; until now every main-thread test off the seat answered 'not main'.", a_seat);
                else
                    spdlog::error("[mainthread] MAIN THREAD MISMATCH: the player Update runs on thread {} but the recorded main "
                                  "thread is {}. Every main-thread test since that record answered for thread {} (engine writes "
                                  "posted instead of run in place, live reads refused, false INVARIANTS #83 / #84 lines). The "
                                  "player Update's thread is the main thread from this frame on. Said for the first 4 changes.",
                                  a_seat, a_recorded, a_recorded);
            } catch (...) {}
        }
        // The other half, once per process: the first player Update ran on the recorded thread (principle 5: observed, not assumed).
        std::atomic<bool> g_seatConfirmed{ false };
        void NoteSeatConfirmed(std::uint32_t a_seat) noexcept {
            if (g_seatConfirmed.exchange(true, std::memory_order_relaxed)) return;
            try {
                spdlog::info("[mainthread] first player Update on thread {}: it is the recorded main thread", a_seat);
            } catch (...) {}
        }

        // ── P0 OVERLAP PROBE (stability audit 2026-11-16, principle 5) ──
        // g_seatSeq: +1 at the thunk's entry and +1 at its exit, so ODD = the main thread is inside the thunk now, and a value that
        // moved between two reads = the thunk started or ended in between. g_seatPhase: 0 outside, 1 = the engine's own
        // PlayerCharacter::Update, 2 = MFO's drain. g_npcIn / g_npcSeq: the same pair for Character::UpdateCombat (Targeting's hook).
        std::atomic<std::uint32_t> g_seatSeq{ 0 };
        std::atomic<int>           g_seatPhase{ 0 };
        std::atomic<int>           g_npcIn{ 0 };
        std::atomic<std::uint32_t> g_npcSeq{ 0 };
        // The reverse sample: how many thunk entries / exits found a worker tick in flight (g_workerTicksInFlight > 0).
        std::atomic<int>           g_workerTicksInFlight{ 0 };
        std::atomic<std::uint32_t> g_seatSawWorker{ 0 };

        struct SeatScope {
            SeatScope() {
                t_inSeat = true;
                const auto me  = static_cast<std::uint32_t>(::GetCurrentThreadId());
                const auto rec = g_mainTid.load(std::memory_order_relaxed);
                if (rec != me) [[unlikely]] NoteSeatThread(rec, me);
                else if (!g_seatConfirmed.load(std::memory_order_relaxed)) [[unlikely]] NoteSeatConfirmed(me);
                g_seatPhase.store(1, std::memory_order_relaxed);
                g_seatSeq.fetch_add(1, std::memory_order_acq_rel);
                if (g_workerTicksInFlight.load(std::memory_order_acquire) > 0) g_seatSawWorker.fetch_add(1, std::memory_order_relaxed);
            }
            ~SeatScope() {
                if (g_workerTicksInFlight.load(std::memory_order_acquire) > 0) g_seatSawWorker.fetch_add(1, std::memory_order_relaxed);
                g_seatSeq.fetch_add(1, std::memory_order_acq_rel);
                g_seatPhase.store(0, std::memory_order_relaxed);
                t_inSeat = false;
            }
        };

        // The pump's fault count (PumpFaults) and its rate-limited ERROR: the first 8, then every 256th. Main thread only writes.
        std::atomic<std::uint64_t> g_pumpFaults{ 0 };
        void NotePumpFault(const char* a_what) noexcept {
            const auto n = g_pumpFaults.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n > 8 && (n % 256) != 0) return;
            try {
                spdlog::error("[mainthread] a posted closure THREW on the main thread: {} (fault #{}). That closure's work is LOST; the pump "
                              "keeps draining the others. Said for the first 8 faults, then every 256th.", a_what ? a_what : "?", n);
            } catch (...) {}   // the log itself failed (out of memory): the count still says it
        }

        // The per-closure guard both drains share (the frame drain below, DrainForSave).
        void RunBatch(std::vector<std::function<void()>>& a_batch) {
            for (auto& fn : a_batch) {
                if (!fn) continue;
                try {
                    fn();
                } catch (const std::exception& e) {
                    NotePumpFault(e.what());
                } catch (...) {
                    NotePumpFault("a non-std exception");
                }
            }
        }

        struct PlayerUpdateHook {
            static void thunk(RE::PlayerCharacter* a_this, float a_delta) {
                // Original FIRST, always -- the pump piggybacks on the engine's
                // frame; it never runs ahead of the engine's own bookkeeping.
                // Same doctrine as Targeting's UpdateCombat thunk.
                SeatScope seat;   // INVARIANTS #84 + the P0 probe: this thread is in the main seat until the thunk returns
                func(a_this, a_delta);
                g_seatPhase.store(2, std::memory_order_relaxed);

                // SWAP-THEN-RUN: take the whole queue under the lock, run it
                // outside. A drained fn can then Post() again without
                // deadlocking on the mutex it just came out of -- the repost
                // simply waits for the next frame.
                std::vector<std::function<void()>> batch;
                {
                    std::lock_guard lk(g_mx);
                    if (g_queue.empty()) return;   // the every-frame path: one lock, no work
                    batch.swap(g_queue);
                }

                // One-time proof-of-life. The crash stacks put AddTask drains
                // on BSJobs::JobThread; this line prints from inside the
                // player's Update, so its presence in the log IS the evidence
                // that Post()ed work runs on the main thread.
                static bool s_first = true;   // main-thread-only, no atomic needed
                if (s_first) {
                    s_first = false;
                    spdlog::info("[mainthread] first drain: {} fn(s) ran inside player Update -- pump is live on the main thread",
                                 batch.size());
                }

                // PER-TASK GUARD (stability audit M5): a closure that throws (in practice std::bad_alloc, or a std::format /
                // container error in MFO's own code) must not unwind through PlayerCharacter::Update (a CTD with a stack that
                // points at the engine) and must not take the rest of the batch with it. It is caught HERE, said loudly, and the
                // drain goes on. Not a mask (principle 7): the closure's work is lost and the ERROR says so; nothing is retried.
                // Only C++ exceptions: an access violation inside a closure is still a crash, as it must be.
                RunBatch(batch);
            }

            static inline REL::Relocation<decltype(thunk)> func;

            // 0x0AD is Actor::Update(float a_delta) on SE/AE, read from the
            // PINNED CommonLibSSE-NG (vcpkg port 3.7.0 = CharmedBaryon/
            // CommonLibSSE @ c4ab853), include/RE/A/Actor.h:367:
            //   SKYRIM_REL_VR_VIRTUAL void Update(float a_delta);   // 0AD
            // -- NOT guessed, and NOT Targeting's 0xE4 (that is UpdateCombat).
            // VTABLE_PlayerCharacter[0] (include/RE/Offsets_VTABLE.h:2231) is
            // the primary, TESObjectREFR-lineage vtable, where inherited slots
            // keep Actor's indices; writing the slot there hooks ONLY the
            // player -- one call per frame, nothing per-NPC, unlike the
            // Character-wide UpdateCombat hook.
            static constexpr std::size_t idx = 0x0AD;   // Actor::Update(float)
        };

    }

    void Post(std::function<void()> a_fn) {
        if (!a_fn || g_dead.load(std::memory_order_relaxed)) return;
        std::lock_guard lk(g_mx);
        g_queue.push_back(std::move(a_fn));
    }

    void Clear() {
        std::lock_guard lk(g_mx);
        g_queue.clear();
        g_clearGen.fetch_add(1, std::memory_order_acq_rel);
    }
    std::uint32_t ClearGeneration() { return g_clearGen.load(std::memory_order_acquire); }

    std::size_t DrainForSave() {
        if (!IsInstalled()) return 0;   // no pump: Post is a no-op and every write ran in place, so nothing can be queued
        if (!t_inSeat && !IsMainThread()) {
            spdlog::error("[mainthread] kSaveGame drain REFUSED: called on a thread that is not the main thread. Posted engine writes "
                          "stay queued across this save (a weapon unequipped by MFO just before it may load still locked).");
            return 0;
        }
        std::vector<std::function<void()>> batch;
        {
            std::lock_guard lk(g_mx);
            batch.swap(g_queue);   // ONCE: what a closure posts from here on lands in the new queue, not in this batch
        }
        if (batch.empty()) return 0;
        const auto n = batch.size();
        RunBatch(batch);
        spdlog::info("[mainthread] kSaveGame: ran {} queued closure(s) before the engine wrote the save (no posted engine write is "
                     "pending across it)", n);
        return n;
    }

    bool IsInstalled() {
        return g_installed.load() && !g_dead.load();
    }

    bool InSeat() { return t_inSeat; }
    bool IsMainThread() {
        const auto tid = g_mainTid.load(std::memory_order_relaxed);
        return tid != 0 && tid == static_cast<std::uint32_t>(::GetCurrentThreadId());
    }

    namespace {
        // THE MAIN THREAD, FROM THE ENGINE (see g_mainTid). Read once, at Install (kDataLoaded: Main exists since WinMain built it,
        // before it started the InitTESThread this call is on, so the field is long written). VERIFY OR REFUSE:
        //   * exact builds only (Install calls it behind Runtime::Known(): +0x18 / +0x28 were read in Main's constructor on
        //     those three executables; anywhere else the pump is not installed and nothing asks);
        //   * the singleton and the id must be non-zero;
        //   * the OS must agree: the thread that owns the engine's own window (Main +0x18, created and pumped by WinMain) must be
        //     that id. A window that cannot be asked (null, or already destroyed) is said and does not refuse: the id still
        //     comes from the verified field. A window owned by ANOTHER thread refuses.
        // Refused = the id stays unknown (0): IsMainThread() is false off the seat (engine writes are posted, live reads wait)
        // and nothing is judged by NoteLiveRead / NoteEngineWrite until the first player Update names the thread.
        void SeedMainThread() {
            const auto here = static_cast<std::uint32_t>(::GetCurrentThreadId());
            const auto* engine = RE::Main::GetSingleton();
            const std::uint32_t tid = engine ? engine->threadID : 0;
            if (tid == 0) {
                spdlog::error("[mainthread] main thread NOT recorded: {} (this call is on thread {}). Until the first player "
                              "Update, engine writes off the seat are posted and live reads wait.",
                              engine ? "the engine's Main holds thread id 0" : "the engine's Main singleton is null", here);
                return;
            }
            const char* window = "its window's owner thread agrees";
            const std::uint32_t owner = engine->wnd ? static_cast<std::uint32_t>(::GetWindowThreadProcessId(engine->wnd, nullptr)) : 0;
            if (owner == 0) {
                window = engine->wnd ? "its window could not be asked for its owner thread (not cross-checked)"
                                   : "it holds no window yet (not cross-checked)";
            } else if (owner != tid) {
                spdlog::error("[mainthread] main thread NOT recorded: the engine's Main says thread {} but its window is owned by "
                              "thread {} (this call is on thread {}). REFUSED, nothing guessed. Until the first player Update, "
                              "engine writes off the seat are posted and live reads wait.", tid, owner, here);
                return;
            }
            g_mainTid.store(tid, std::memory_order_relaxed);
            spdlog::info("[mainthread] main thread {} (the engine's own record: Main's constructor stored it on the WinMain thread, "
                         "which runs the frame loop; {}). This call (kDataLoaded) is on thread {}{}.", tid, window, here,
                         here == tid ? "" : ": SKSE sends kDataLoaded from the engine's InitTESThread, not from the main thread");
        }
    }
    bool InWorkerTick() { return t_workerTick > 0; }
    bool LiveReadsAllowed() {
        if (!IsInstalled()) return true;                 // no pump (VR / unverified build): the documented legacy path, as before
        if (t_workerTick > 0) return false;              // the worker tick ALWAYS reads snapshots, whichever thread the job system gave it
        if (t_inSeat) return true;                       // the main seat
        return IsMainThread();                           // main-thread code outside the tick (sinks, load handlers): unchanged
    }

    bool EngineWritesAllowed() { return LiveReadsAllowed(); }
    bool RunOrPost(std::function<void()> a_fn) {
        if (!a_fn) return false;
        if (EngineWritesAllowed()) { a_fn(); return true; }
        Post(std::move(a_fn));
        return false;
    }
    std::uint64_t PumpFaults() { return g_pumpFaults.load(std::memory_order_relaxed); }

    namespace {
        std::atomic<std::uint64_t> g_liveReadViolations{ 0 }, g_workerWriteReads{ 0 }, g_engineWriteViolations{ 0 };
        constexpr std::size_t kSaidSites = 64;
        std::atomic<const char*> g_saidSites[kSaidSites]{};
        // TRUE the first time a_site (a string literal / __FUNCTION__: compared by address) is seen. A full table says nothing more.
        bool FirstTime(const char* a_site) {
            for (auto& slot : g_saidSites) {
                const char* cur = slot.load(std::memory_order_relaxed);
                if (cur == a_site) return false;
                if (!cur) {
                    const char* none = nullptr;
                    if (slot.compare_exchange_strong(none, a_site, std::memory_order_relaxed)) return true;
                    if (none == a_site) return false;
                }
            }
            return false;
        }
        // Off the seat in a way that is NOT allowed for a plain live read: inside a worker tick, or on a thread that is provably
        // not the main one. While the main thread is unknown (before SeedMainThread, or after it refused) nothing is judged.
        const char* OffSeat() {
            if (!IsInstalled()) return nullptr;
            if (t_workerTick > 0) return "inside the worker tick";
            if (t_inSeat) return nullptr;
            const auto tid = g_mainTid.load(std::memory_order_relaxed);
            if (tid != 0 && tid != static_cast<std::uint32_t>(::GetCurrentThreadId())) return "on a thread that is not the main thread";
            return nullptr;
        }
    }

    void NoteLiveRead(const char* a_site) {
        const char* why = OffSeat();
        if (!why) return;
        g_liveReadViolations.fetch_add(1, std::memory_order_relaxed);
        if (FirstTime(a_site))
            spdlog::error("[thread] INVARIANTS #84 VIOLATED at '{}': a LIVE engine inventory / active-effect list is read {} (it must run on "
                          "the main thread: MainThread::Post, or read through InvSnap / EffectMirror). The read still ran. Said once per site.",
                          a_site ? a_site : "?", why);
    }
    void NoteWriteRead(const char* a_site) {
        if (!OffSeat()) return;
        g_workerWriteReads.fetch_add(1, std::memory_order_relaxed);
        if (FirstTime(a_site))
            spdlog::info("[thread] '{}': a worker-side inventory WRITE reads the live list it writes (the known residue, stability audit M2: "
                         "the write moves to the main thread with this read). Counted in [overlap]. Said once per site.", a_site ? a_site : "?");
    }
    void NoteEngineWrite(const char* a_site) {
        const char* why = OffSeat();
        if (!why) return;
        g_engineWriteViolations.fetch_add(1, std::memory_order_relaxed);
        if (FirstTime(a_site))
            spdlog::error("[thread] INVARIANTS #83 VIOLATED at '{}': an engine actor / inventory / caster / graph WRITE runs {} (it must run "
                          "on the main thread: EngineOps, or MainThread::Post). The write still ran. Said once per site.",
                          a_site ? a_site : "?", why);
    }
    std::uint64_t EngineWriteViolations() { return g_engineWriteViolations.load(std::memory_order_relaxed); }
    std::uint64_t LiveReadViolations() { return g_liveReadViolations.load(std::memory_order_relaxed); }
    std::uint64_t WorkerWriteReads()   { return g_workerWriteReads.load(std::memory_order_relaxed); }

    WorkerTickScope::WorkerTickScope() {
        ++t_workerTick;
        g_workerTicksInFlight.fetch_add(1, std::memory_order_acq_rel);
    }
    WorkerTickScope::~WorkerTickScope() {
        g_workerTicksInFlight.fetch_sub(1, std::memory_order_acq_rel);
        --t_workerTick;
    }

    std::uint32_t SeatSeq()        { return g_seatSeq.load(std::memory_order_acquire); }
    int           SeatPhase()      { return g_seatPhase.load(std::memory_order_relaxed); }
    std::uint32_t SeatSawWorker()  { return g_seatSawWorker.load(std::memory_order_relaxed); }
    std::uint32_t NpcCombatSeq()   { return g_npcSeq.load(std::memory_order_acquire); }
    int           NpcCombatIn()    { return g_npcIn.load(std::memory_order_acquire); }
    NpcCombatScope::NpcCombatScope()  { g_npcIn.fetch_add(1, std::memory_order_acq_rel); g_npcSeq.fetch_add(1, std::memory_order_acq_rel); }
    NpcCombatScope::~NpcCombatScope() { g_npcIn.fetch_sub(1, std::memory_order_acq_rel); }

    void Install() {
        // SKYRIM_REL_VR_VIRTUAL: on VR the Update slot sits at a DIFFERENT
        // index, and writing 0x0AD there would vector every frame into an
        // arbitrary virtual -- an instant CTD pointing nowhere near MFO. Same
        // refusal, for the same reason, as Targeting's UpdateCombat hook.
        // G1: EXACT builds only (was VR-only). Packages / Rapport branch on the
        // same Runtime::Known() to pick AddTask over a dead pump.
        if (!Runtime::Known()) {
            g_dead = true;
            spdlog::warn("[mainthread] runtime {} not supported ({}) -- Update vtable index is not "
                         "verified there; pump NOT installed (Post becomes a no-op).",
                         REL::Module::get().version().string("."), Runtime::GateReason());
            return;
        }
        if (g_installed.exchange(true)) return;

        // The first kPostLoadGame runs before the first player Update, so the main thread must be known NOW. kDataLoaded is NOT
        // on it (SKSE sends it from InitTESThread): the id comes from the engine's own record, verified or refused.
        SeedMainThread();

        // A VTABLE INDEX, not an AddressLib offset -- version-resilient the
        // same way Targeting's hook is (§0.12's lesson).
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
        if (!Runtime::SeatVerified(vtbl.address(), "MainThread.PlayerCharacter.Update")) {
            g_dead = true;
            spdlog::error("[mainthread] player Update hook NOT installed (self-check refused the PlayerCharacter "
                          "vtable); Post becomes a no-op.");
            return;
        }
        PlayerUpdateHook::func =
            vtbl.write_vfunc(PlayerUpdateHook::idx, PlayerUpdateHook::thunk);

        spdlog::info("[mainthread] player Update vfunc hook installed (PlayerCharacter vtbl idx 0x{:X})",
                     PlayerUpdateHook::idx);
    }

}
